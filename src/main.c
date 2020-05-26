/***************************************************************************
 * This file is part of NUSspli.                                           *
 * Copyright (c) 2019-2020 Pokes303                                        *
 * Copyright (c) 2020 V10lator <v10lator@myway.de>                         *
 *                                                                         *
 * This program is free software; you can redistribute it and/or modify    *
 * it under the terms of the GNU General Public License as published by    *
 * the Free Software Foundation; either version 2 of the License, or       *
 * (at your option) any later version.                                     *
 *                                                                         *
 * This program is distributed in the hope that it will be useful,         *
 * but WITHOUT ANY WARRANTY; without even the implied warranty of          *
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the           *
 * GNU General Public License for more details.                            *
 *                                                                         *
 * You should have received a copy of the GNU General Public License along *
 * with this program; if not, write to the Free Software Foundation, Inc., *
 * 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.             *
 ***************************************************************************/

#include <wut-fixups.h>

#include <config.h>
#include <file.h>
#include <input.h>
#include <installer.h>
#include <ioThread.h>
#include <main.h>
#include <memdebug.h>
#include <renderer.h>
#include <status.h>
#include <ticket.h>
#include <usb.h>
#include <utils.h>
#include <cJSON.h>
#include <menu/download.h>
#include <menu/main.h>
#include <menu/utils.h>

#include <errno.h>
#include <limits.h>
#include <math.h>
#include <string.h>
#include <sys/stat.h>

#include <coreinit/filesystem.h>
#include <coreinit/foreground.h>
#include <coreinit/mcp.h>
#include <coreinit/memdefaultheap.h>
#include <coreinit/memfrmheap.h>
#include <coreinit/memheap.h>
#include <coreinit/memory.h>
#include <coreinit/thread.h>
#include <coreinit/time.h>
#include <coreinit/title.h>
#include <curl/curl.h>
#include <nn/ac/ac_c.h>
#include <nn/result.h>
#include <proc_ui/procui.h>
#include <whb/crash.h>
#include <whb/proc.h>

bool hbl;

FSClient fsCli;
FSCmdBlock fsCmd;

uint16_t contents; //Contents count
uint16_t dcontent; //Actual content number

double downloaded;

char *ramBuf = NULL;
size_t ramBufSize = 0;

char *downloading = "UNKNOWN";
bool downloadPaused = false;
OSTime lastDraw = 0;

static size_t headerCallback(void *buf, size_t size, size_t multi, void *rawData)
{
	size *= multi;
	((char *)buf)[size - 1] = '\0'; //TODO: This should transform the newline at the end to a string terminator - but are we allowed to modify the buffer?
	
	if(rawData == NULL)
		return size;
	
	toLowercase(buf);
	long contentLength = 0;
	if(sscanf(buf, "content-length: %ld", &contentLength) != 1)
		return size;
	
	if(*(long *)rawData == contentLength)
	{
		*(long *)rawData = -1;
		return 0;
	}
	return size;
}

static int progressCallback(void *curl, double dltotal, double dlnow, double ultotal, double ulnow) {
	OSTime now = OSGetSystemTime();
	if(lastDraw > 0)
	{
		if(OSTicksToMilliseconds(now - lastDraw) < 1000)
			return 0;
	}
	lastDraw = now;
	
	if(AppRunning())
	{
		if(app == 2)
		{
			if(!downloadPaused && curl_easy_pause(curl, CURLPAUSE_ALL) == CURLE_OK)
			{
				downloadPaused = true;
				debugPrintf("Download paused!");
			}
			return 0;
		}
		else if(downloadPaused && curl_easy_pause(curl, CURLPAUSE_CONT) == CURLE_OK)
		{
			downloadPaused = false;
			debugPrintf("Download resumed");
		}
	}
	else
	{
		debugPrintf("Download cancelled!");
		return 1;
	}
	
	// debugPrintf("Downloading: %s (%u/%u) [%u%%] %u / %u bytes", downloading, dcontent, contents, (uint32_t)(dlnow / ((dltotal > 0) ? dltotal : 1) * 100), (uint32_t)dlnow, (uint32_t)dltotal);
	startNewFrame();
	char tmpString[13 + strlen(downloading)];
	if (dltotal == 0 || isinf(dltotal) || isnan(dltotal))
	{
		strcpy(tmpString, "Preparing ");
		strcat(tmpString, downloading);
		textToFrame(0, 0, tmpString);
	}
	else
	{
		strcpy(tmpString, "Downloading ");
		strcat(tmpString, downloading);
		textToFrame(0, 0, tmpString);
		
		int multiplier;
		char *multiplierName;
		if(dltotal < 1024.0D)
		{
			multiplier = 1;
			multiplierName = "B";
		}
		else if(dltotal < 1024.0D * 1024.0D)
		{
			multiplier = 1 << 10;
			multiplierName = "KB";
		}
		else if(dltotal < 1024.0D * 1024.0D * 1024.0D)
		{
			multiplier = 1 << 20;
			multiplierName = "MB";
		}
		else
		{
			multiplier = 1 << 30;
			multiplierName = "GB";
		}
		barToFrame(1, 0, 40, (float)(dlnow / dltotal) * 100.0f);
		sprintf(tmpString, "%.2f / %.2f %s", dlnow / multiplier, dltotal / multiplier, multiplierName);
		textToFrame(41, 1, tmpString);
	}
	
	if(contents < 0xFFFF)
	{
		sprintf(tmpString, "(%d/%d)", dcontent + 1, contents);
		textToFrame(ALIGNED_RIGHT, 0, tmpString);
	}
	
	if(dlnow != 0)
	{
		char buf[32];
		getSpeedString(dlnow - downloaded, buf);
		
		downloaded = dlnow;
		textToFrame(ALIGNED_RIGHT, 1, buf);
	}
	
	writeScreenLog();
	drawFrame();
	showFrame();
	return 0;
}

int downloadFile(char* url, char* file, FileType type) {
	//Results: 0 = OK | 1 = Error | 2 = No ticket aviable | 3 = Exit
	//Types: 0 = .app | 1 = .h3 | 2 = title.tmd | 3 = tilte.tik
	bool toRam = (type & FILE_TYPE_TORAM) == FILE_TYPE_TORAM;
	
	debugPrintf("Download URL: %s", url);
	debugPrintf("Download PATH: %s", toRam ? "<RAM>" : file);
	
	if(toRam)
		downloading = file;
	else
	{
		int haystack;
		for(haystack = strlen(file); file[haystack] != '/'; haystack--)
		{
		}
		downloading = &file[++haystack];
	}
	
	downloaded = 0.0D;
	
	bool fileExists;
	FILE *fp;
	if(toRam)
		fp = open_memstream(&ramBuf, &ramBufSize);
	else
	{
		fileExists = pathExists(file);
		fp = fopen(file, fileExists ? "rb+" : "wb");
	}
	
	CURL *curl = curl_easy_init();
	if(curl == NULL)
	{
		fclose(fp);
		
		char err[128];
		sprintf(err, "ERROR: curl_easy_init failed\nFile: %s", file);
		drawErrorFrame(err, B_RETURN);
		
		while(true)
		{
			showFrame();
			if(vpad.trigger ==  VPAD_BUTTON_B)
				return 1;
		}
	}
	
	CURLcode ret = curl_easy_setopt(curl, CURLOPT_URL, url);
	
	char curlError[CURL_ERROR_SIZE];
	curl_easy_setopt(curl, CURLOPT_ERRORBUFFER, curlError);
	curlError[0] = '\0';
	
	{
		//TODO: Spoof eShop here?
		char ua[128];
		strcpy(ua, "NUSspli/");
		strcat(ua, NUSSPLI_VERSION);
		strcat(ua, " (WarezLoader, like WUPDownloader)");
		ret |= curl_easy_setopt(curl, CURLOPT_USERAGENT, ua);
	}
	
	long fileSize;
	if(!toRam && fileExists)
	{
		fileSize = getFilesize(fp);
		ret |= curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, headerCallback);
		ret |= curl_easy_setopt(curl, CURLOPT_WRITEHEADER, &fileSize);
	}
	
	ret |= curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, toRam ? fwrite : addToIOQueue);
    ret |= curl_easy_setopt(curl, CURLOPT_WRITEDATA, fp);

	ret |= curl_easy_setopt(curl, CURLOPT_PROGRESSFUNCTION, progressCallback);
	ret |= curl_easy_setopt(curl, CURLOPT_PROGRESSDATA, curl);
	ret |= curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
	
	ret |= curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
	
	if(ret != CURLE_OK)
	{
		fclose(fp);
		curl_easy_cleanup(curl);
		debugPrintf("curl_easy_setopt error: %s", curlError);
		return 1;
	}
	
	debugPrintf("Calling curl_easy_perform()");
	ret = curl_easy_perform(curl);
	debugPrintf("curl_easy_perform() returned: %d", ret);
	
	if(toRam)
	{
		fflush(fp);
		fclose(fp);
		
		long resp;
		if(ret == CURLE_OK)
			curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &resp);
		else
			resp = 0;
		debugPrintf("CURL returned: %d / %d", ret, resp);
		curl_easy_cleanup(curl);
		return ret | (resp == 200 ? 0 : resp);
	}
	
	addToIOQueue(NULL, 0, 0, fp);
	
	if(ret != CURLE_OK)
	{
		debugPrintf("curl_easy_perform returned an error: %s (%d)\nFile: %s\n\n", curlError, ret, file);
		curl_easy_cleanup(curl);
		remove(file);
		
		char toScreen[1024];
		sprintf(toScreen, "curl_easy_perform returned a non-valid value: %d\n\n", ret);
		switch(ret) {
			case CURLE_FAILED_INIT:
			case CURLE_COULDNT_RESOLVE_HOST:
				strcat(toScreen, "---> Network error\nYour WiiU is not connected to the internet,\ncheck the network settings and try again");
				break;
			case CURLE_WRITE_ERROR:
				strcat(toScreen, "---> Error from SD Card\nThe SD Card was extracted or invalid to write data,\nre-insert it or use another one and restart the app");
				break;
			case CURLE_OPERATION_TIMEDOUT:
			case CURLE_GOT_NOTHING:
			case CURLE_SEND_ERROR:
			case CURLE_RECV_ERROR:
			case CURLE_PARTIAL_FILE:
				strcat(toScreen, "---> Network error\nFailed while trying to download data, probably your router was turned off,\ncheck the internet connecition and try again");
				break;
			case CURLE_ABORTED_BY_CALLBACK:
				return 0;
			default:
				strcat(toScreen, "---> Unknown error\n");
				strcat(toScreen, curlError);
				break;
		}
		
		drawErrorFrame(toScreen, B_RETURN | Y_RETRY);
		
		while(true)
		{
			showFrame();
			
			switch (vpad.trigger) {
				case VPAD_BUTTON_B:
					return 1;
				case VPAD_BUTTON_Y:
					return downloadFile(url, file, type);
			}
		
		}
	}
	debugPrintf("curl_easy_perform executed successfully");
	
	if(fileSize == -1)
	{
		curl_easy_cleanup(curl);
		addToScreenLog("Download %s skipped!", downloading);
		return 0;
	}

	long resp;
	ret = curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &resp);
	curl_easy_cleanup(curl);
	
	debugPrintf("The download returned: %u", resp);
	if (resp != 200) {
		remove(file);
		if(resp == 404 && (type & FILE_TYPE_TMD) == FILE_TYPE_TMD) //Title.tmd not found
		{
			drawErrorFrame("The download of title.tmd failed with error: 404\n\nThe title cannot be found on the NUS, maybe the provided title ID doesn't exists or\nthe TMD was deleted", B_RETURN | Y_RETRY);
			
			while(true)
			{
				showFrame();

				switch(vpad.trigger)
				{
					case VPAD_BUTTON_B:
						return 1;
					case VPAD_BUTTON_Y:
						return downloadFile(url, file, type);
				}
			}
		}
		else if (resp == 404 && (type & FILE_TYPE_TIK) == FILE_TYPE_TIK) { //Fake ticket needed
			return 2;
		}
		else
		{
			char toScreen[1024];
			sprintf(toScreen, "The download returned a result different to 200 (OK): %ld\nFile: %s\n\n", resp, file);
			if(resp == 400)
				strcat(toScreen, "Request failed. Try again\n\n");
			drawErrorFrame(toScreen, B_RETURN | Y_RETRY);
			
			while(true)
			{
				showFrame();
				
				switch (vpad.trigger) {
					case VPAD_BUTTON_B:
						return 1;
					case VPAD_BUTTON_Y:
						return downloadFile(url, file, type);
				}
			}
		}
	}
	
	debugPrintf("The file was downloaded successfully");
	addToScreenLog("Download %s finished!", downloading);
	
	return 0;
}

bool downloadTitle(GameInfo game, char* titleVer, char* folderName, bool inst, bool dlToUSB, bool toUSB, bool keepFiles)
{
	debugPrintf("Downloading title... tID: %s, tVer: %s, name: %s, folder: %s", game.tid, titleVer, game.name == NULL ? "NULL" : game.name, folderName);
	disableShutdown(); //TODO
	
	char downloadUrl[128];
	strcpy(downloadUrl, DOWNLOAD_URL);
	strcat(downloadUrl, game.tid);
	strcat(downloadUrl, "/");
	
	if(strlen(folderName) == 0)
		folderName = game.tid;
	else {
		strcat(folderName, " [");
		strcat(folderName, game.tid);
		strcat(folderName, "]");
	}
	if (strlen(titleVer) > 0) {
		strcat(folderName, " [v");
		strcat(folderName, titleVer);
		strcat(folderName, "]");
	}
	
	char installDir[FILENAME_MAX + 37];
	strcpy(installDir, dlToUSB ? INSTALL_DIR_USB : INSTALL_DIR_SD);
	if(!dirExists(installDir))
	{
		debugPrintf("Creating directroty \"%s\"", installDir);
		mkdir(installDir, 777);
	}
	
	strcat(installDir, folderName);
	strcat(installDir, "/");
	
	if(game.name == NULL)
		game.name = game.tid;
	
	addToScreenLog("Started the download of \"%s\"", game.name);
	addToScreenLog("The content will be saved on \"%s:/install/%s\"", dlToUSB ? "usb" : "sd", folderName);
	
	if(!dirExists(installDir))
	{
		debugPrintf("Creating directory");
		errno = 0;
		if(mkdir(installDir, 777) == -1)
		{
			addToScreenLog("Error creating directory: %d %s", errno, strerror(errno));
			return false; // TODO
		}
		else
			addToScreenLog("Download directory successfully created");
	}
	else
		addToScreenLog("WARNING: The download directory already exists");
	
	debugPrintf("Downloading TMD...");
	char tDownloadUrl[256];
	strcpy(tDownloadUrl, downloadUrl);
	strcat(tDownloadUrl, "tmd");
	if(strlen(titleVer) > 0)
	{
		strcat(tDownloadUrl, ".");
		strcat(tDownloadUrl, titleVer);
	}
	char tmd[FILENAME_MAX + 37];
	strcpy(tmd, installDir);
	strcat(tmd, "title.tmd");
	contents = 0xFFFF;
	if(downloadFile(tDownloadUrl, tmd, FILE_TYPE_TMD))
	{
		debugPrintf("Error downloading TMD");
		return true;
	}
	flushIOQueue();
	addToScreenLog("TMD Downloaded");
	
	char toScreen[128];
	strcpy(toScreen, "=>Title type: ");
	bool hasDependencies;
	switch (readUInt32(tmd, 0x18C)) { //Title type
		case 0x00050000:
			strcat(toScreen, "eShop or Packed");
			hasDependencies = false;
			break;
		case 0x00050002:
			strcat(toScreen, "eShop/Kiosk demo");
			hasDependencies = false;
			break;
		case 0x0005000C:
			strcat(toScreen, "eShop DLC");
			hasDependencies = true;
			break;
		case 0x0005000E:
			strcat(toScreen, "eShop Update");
			hasDependencies = true;
			break;
		case 0x00050010:
			strcat(toScreen, "System Application");
			hasDependencies = false;
			break;
		case 0x0005001B:
			strcat(toScreen, "System Data Archive");
			hasDependencies = false;
			break;
		case 0x00050030:
			strcat(toScreen, "Applet");
			hasDependencies = false;
			break;
		// vWii //
		case 0x7:
			strcat(toScreen, "Wii IOS");
			hasDependencies = false;
			break;
		case 0x00070002:
			strcat(toScreen, "vWii Default Channel");
			hasDependencies = false;
			break;
		case 0x00070008:
			strcat(toScreen, "vWii System Channel");
			hasDependencies = false;
			break;
		default:
			strcat(toScreen, "Unknown (");
			char *h = hex(readUInt32(tmd, 0x18C), 8);
			strcat(toScreen, h);
			MEMFreeToDefaultHeap(h);
			strcat(toScreen, ")");
			hasDependencies = false;
			break;
	}
	addToScreenLog(toScreen);
	strcpy(tDownloadUrl, downloadUrl);
	strcat(tDownloadUrl, "cetk");
	
	char tInstallDir[FILENAME_MAX + 37];
	strcpy(tInstallDir, installDir);
	strcat(tInstallDir, "title.tik");
	int tikRes = downloadFile(tDownloadUrl, tInstallDir, FILE_TYPE_TIK);
	if(tikRes == 1)
		return true;
	if(tikRes == 2)
	{
		addToScreenLog("Title.tik not found on the NUS. Checking known keys...");
		
		if(game.key != NULL && game.key[0] == 'x')
		{
			strcpy(tInstallDir, game.tid);
			strcat(tInstallDir, ".tik");
			
			strcpy(tDownloadUrl, getTitleKeySite());
			strcat(tDownloadUrl, "/ticket/");
			strcat(tDownloadUrl, tInstallDir);
			if(downloadFile(tDownloadUrl, tInstallDir, FILE_TYPE_TIK | FILE_TYPE_TORAM) == 0)
			{
				game.key = hex(readUInt64(tmd, 0x1BF), 16);
				debugPrintf("Extracted key: %s", game.key);
			}
			
			if(ramBuf != NULL)
			{
				MEMFreeToDefaultHeap(ramBuf);
				ramBuf = NULL;
				ramBufSize = 0;
			}
		}
		
		if(game.key == NULL || game.key[0] == 'x')
		{
			colorStartNewFrame(SCREEN_COLOR_LILA);
			strcpy(toScreen, "Input the Encrypted title key of ");
			strcat(toScreen, game.name);
			strcat(toScreen, " to create a");
			textToFrame(0, 0, toScreen);
			textToFrame(1, 1, "fake ticket (Without it, the download cannot be installed)");
			textToFrame(0, 3, "You can get it from any 'WiiU title key site'");
			textToFrame(0, 5, "Press \uE000 to show the keyboard [32 hexadecimal numbers]");
			textToFrame(0, 6, "Press \uE001 to continue the download without the fake ticket");
			drawFrame();
			
			char encKey[33];
			while(true)
			{
				showFrame();
				
				if(vpad.trigger == VPAD_BUTTON_A)
				{
					if(showKeyboard(KEYBOARD_TYPE_RESTRICTED, encKey, CHECK_HEXADECIMAL, 32, true, NULL, "CREATE"))
					{
						game.key = encKey;
						break;
					}
				}
				else if(vpad.trigger == VPAD_BUTTON_B)
				{
					addToScreenLog("Encrypted key wasn't wrote. Continuing without fake ticket");
					addToScreenLog("(The download needs a fake ticket to be installed)");
					break;
				}
			}
		}
		
		if(game.key != NULL || game.key[0] == 'x')
		{
			startNewFrame();
			textToFrame(0, 0, "Creating fake title.tik");
			writeScreenLog();
			drawFrame();
			showFrame();
			
			FILE *tik = fopen(tInstallDir, "wb"); //TODO: Error checking
			generateTik(tik, game.tid, game.key);
			fflush(tik);
			fclose(tik);
			addToScreenLog("Fake ticket created successfully");
		}
	}

	uint16_t conts = readUInt16(tmd, 0x1DE);
	char *apps[conts];
	bool h3[conts];
	contents = conts;
	
	//Get .app and .h3 files
	for(int i = 0; i < conts; i++)
	{
		apps[i] = hex(readUInt32(tmd, 0xB04 + i * 0x30), 8);
		h3[i] = readUInt16(tmd, 0xB0A + i * 0x30) == 0x2003;
		if(h3[i])
			contents++;
	}
	
	char tmpFileName[FILENAME_MAX + 37];
	dcontent = 0;
	for(int i = 0; i < conts; i++) {
		strcpy(tDownloadUrl, downloadUrl);
		strcat(tDownloadUrl, apps[i]);
		strcpy(tmpFileName, installDir);
		strcat(tmpFileName, apps[i]);
		MEMFreeToDefaultHeap(apps[i]);
		
		strcpy(tInstallDir, tmpFileName);
		strcat(tInstallDir, ".app");
		if(downloadFile(tDownloadUrl, tInstallDir, FILE_TYPE_APP) == 1)
		{
			for(int j = ++i; j < conts; j++)
				MEMFreeToDefaultHeap(apps[j]);
			return true;
		}
		dcontent++;
		
		if(h3[i])
		{
			strcat(tDownloadUrl, ".h3");
			strcat(tmpFileName, ".h3");
			if(downloadFile(tDownloadUrl, tmpFileName, FILE_TYPE_H3) == 1)
			{
				for(int j = ++i; j < conts; j++)
					MEMFreeToDefaultHeap(apps[j]);
				return true;
			}
			dcontent++;
		}
	}
	
	strcpy(tmpFileName, installDir);
	strcat(tmpFileName, "title.cert");
	
	if(!fileExists(tmpFileName))
	{
		debugPrintf("Creating CERT...");
		startNewFrame();
		textToFrame(0, 0, "Creating CERT");
		writeScreenLog();
		drawFrame();
		showFrame();
		
		FILE *cert = fopen(tmpFileName, "wb");
		
		// NUSspli adds its own header.
		writeHeader(cert, FILE_TYPE_CERT);
		
		// Some SSH certificate
		writeCustomBytes(cert, "0x526F6F742D43413030303030303033"); // "Root-CA00000003"
		writeVoidBytes(cert, 0x34);
		writeCustomBytes(cert, "0x0143503030303030303062"); // "?CP0000000b"
		writeVoidBytes(cert, 0x36);
		writeRandomBytes(cert, 0x104);
		writeCustomBytes(cert, "0x00010001");
		writeVoidBytes(cert, 0x34);
		writeCustomBytes(cert, "0x00010003");
		writeRandomBytes(cert, 0x200);
		writeVoidBytes(cert, 0x3C);
		
		// Next certificate
		writeCustomBytes(cert, "0x526F6F74"); // "Root"
		writeVoidBytes(cert, 0x3F);
		writeCustomBytes(cert, "0x0143413030303030303033"); // "?CA00000003"
		writeVoidBytes(cert, 0x36);
		writeRandomBytes(cert, 0x104);
		writeCustomBytes(cert, "0x00010001");
		writeVoidBytes(cert, 0x34);
		writeCustomBytes(cert, "0x00010004");
		writeRandomBytes(cert, 0x100);
		writeVoidBytes(cert, 0x3C);
		
		// Last certificate
		writeCustomBytes(cert, "0x526F6F742D43413030303030303033"); // "Root-CA00000003"
		writeVoidBytes(cert, 0x34);
		writeCustomBytes(cert, "0x0158533030303030303063"); // "?XS0000000c"
		writeVoidBytes(cert, 0x36);
		writeRandomBytes(cert, 0x104);
		writeCustomBytes(cert, "0x00010001");
		writeVoidBytes(cert, 0x34);
		
		fflush(cert);
		fclose(cert);
		
		addToScreenLog("Cert created!");
	}
	else
		addToScreenLog("Cert skipped!");
	
	flushIOQueue();
	if(inst)
		return install(game.name, hasDependencies, dlToUSB, installDir, toUSB, keepFiles);
	
	enableShutdown(); //TODO
	
	colorStartNewFrame(SCREEN_COLOR_GREEN);
	textToFrame(0, 0, game.name);
	textToFrame(0, 1, "Downloaded successfully!");
	writeScreenLog();
	drawFrame();
	showFrame();
	
	for(int i = 0; i < 0x10; i++)
		VPADControlMotor(VPAD_CHAN_0, vibrationPattern, 0xF);
	
	while(AppRunning() && (app == 2 || (vpad.trigger != VPAD_BUTTON_A && vpad.trigger != VPAD_BUTTON_B)))
		showFrame();
	
	return true;
}

int main()
{
	// Init
	hbl = OSGetTitleID() == 0x0005000013374842 || // HBL Channel
			OSGetTitleID() == 0x000500101004A200 ||  // Mii Maker EUR
			OSGetTitleID() == 0x000500101004A100 || // Mii Maker USA
			OSGetTitleID() == 0x000500101004A000 || // Mii Maker JPN
			OSGetTitleID() == 0;
	
	if(hbl)
		WHBProcInit();
	else
		ProcUIInit(&OSSavesDone_ReadyToRelease);
	
	OSThread *mainThread = OSGetCurrentThread();
	OSSetThreadName(mainThread, "NUSspli");
	
	debugInit();
	debugPrintf("main()");
#ifdef NUSSPLI_DEBUG
	WHBInitCrashHandler();
	OSCheckActiveThreads();
#endif
	
	initRenderer();
	
	// ASAN Trigger
/*	char *asanHeapTrigger = MEMAllocFromDefaultHeap(1);
	debugPrintf("ASAN Debug: Triggering buffer-read overflow:");
	if(asanHeapTrigger[1] == ' ') ;
	debugPrintf("ASAN Debug: Triggering buffer-write overflow:");
	asanHeapTrigger[1] = '?';
	debugPrintf("ASAN Debug: Triggering double-free:");
	MEMFreeToDefaultHeap(asanHeapTrigger);
	MEMFreeToDefaultHeap(asanHeapTrigger);*/
	
	if(OSSetThreadPriority(mainThread, 1))
		addToScreenLog("Changed main thread priority!");
	else
		addToScreenLog("WARNING: Error changing main thread priority!");
	startNewFrame();
	textToFrame(0, 0, "Seeding RNG...");
	writeScreenLog();
	drawFrame();
	showFrame();
	
	initRandom();
	
	addToScreenLog("RNG seeded!");
	startNewFrame();
	textToFrame(0, 0, "Initializing filesystem...");
	writeScreenLog();
	drawFrame();
	showFrame();
	
	char *lerr = NULL;
	
	FSInit(); // We need to start this before the SWKBD.
	if(FSAddClient(&fsCli, 0) == FS_STATUS_OK)
	{
		addToScreenLog("Filesystem initialized!");
		startNewFrame();
		textToFrame(0, 0, "Loading network...");
		writeScreenLog();
		drawFrame();
		showFrame();
		
		if(NNResult_IsSuccess(ACInitialize()))
		{
			ACConfigId networkID;
			if(NNResult_IsSuccess(ACGetStartupId(&networkID)))
			{
				ACConnectWithConfigId(networkID);
				addToScreenLog("Network initialized!");
				
				startNewFrame();
				textToFrame(0, 0, "Loading cJSON...");
				writeScreenLog();
				drawFrame();
				showFrame();
				
				cJSON_Hooks ch;
				ch.malloc_fn = MEMAllocFromDefaultHeap;
				ch.free_fn = MEMFreeToDefaultHeap;
				cJSON_InitHooks(&ch);
				
				addToScreenLog("cJSON initialized!");
				startNewFrame();
				textToFrame(0, 0, "Loading SWKBD...");
				writeScreenLog();
				drawFrame();
				showFrame();
				
				if(SWKBD_Init())
				{
					addToScreenLog("SWKBD initialized!");
					startNewFrame();
					textToFrame(0, 0, "Loading MCP...");
					writeScreenLog();
					drawFrame();
					showFrame();
					
					mcpHandle = MCP_Open();
					if(mcpHandle != 0)
					{
						addToScreenLog("MCP initialized!");
						startNewFrame();
						textToFrame(0, 0, "Loading I/O thread...");
						writeScreenLog();
						drawFrame();
						showFrame();
						
						if(initIOThread())
						{
							addToScreenLog("I/O thread initialized!");
							startNewFrame();
							textToFrame(0, 0, "Loading config file...");
							writeScreenLog();
							drawFrame();
							showFrame();
							
							#ifdef NUSSPLI_DEBUG
							debugPrintf("Checking thread stacks...");
							OSCheckActiveThreads();
							#endif
						
							if(initConfig())
							{
								if(downloadJSON())
									// Main loop
									mainMenu();
								
								debugPrintf("Deinitializing libraries...");
								saveConfig();
								
								#ifdef NUSSPLI_DEBUG
								debugPrintf("Checking thread stacks...");
								OSCheckActiveThreads();
								#endif
							}
							else
								lerr = "Couldn't load config file!";
							
							shutdownIOThread();
							debugPrintf("I/O thread closed");
						}
						else
							lerr = "Couldn't load I/O thread!";
						
						MCP_Close(mcpHandle);
						debugPrintf("MCP closed");
					}
					else
						lerr = "Couldn't initialize MCP!";
					
					SWKBD_Shutdown();
					debugPrintf("SWKBD closed");
				}
				else
					lerr = "Couldn't initialize SWKBD!";
				
				freeJSON();
			}
			else
				lerr = "Couldn't get default network connection!";
			
			ACFinalize();
			debugPrintf("Network closed");
		}
		else
			lerr = "Couldn't inititalize network!";
		
		unmountUSB();
		FSDelClient(&fsCli, 0);
		FSShutdown();
		debugPrintf("FS closed");
	}
	else
		lerr = "Couldn't initialize filesystem!";
	
	if(lerr != NULL)
	{
		drawErrorFrame(lerr, B_RETURN);
			
		while(vpad.trigger != VPAD_BUTTON_B)
			showFrame();
	} 
	
	shutdownRenderer();
	clearScreenLog();
	debugPrintf("libgui closed");
	
#ifdef NUSSPLI_DEBUG
	debugPrintf("Checking thread stacks...");
	OSCheckActiveThreads();
#endif
	
	shutdownDebug();
	
	if(hbl)
		WHBProcShutdown();
	else
		ProcUIShutdown();
	
	deinitASAN();
	return 0;
}

void __preinit_user(MEMHeapHandle *mem1, MEMHeapHandle *fg, MEMHeapHandle *mem2)
{
	debugInit();
	debugPrintf("__preinit_user()");
	initASAN(*mem2);
	shutdownDebug();
}
