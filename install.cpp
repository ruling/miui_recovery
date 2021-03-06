/*
 * Copyright (C) 2007 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include "common.h"
#include "install.h"
extern "C" {
#include "mincrypt/rsa.h"
#include "minui/minui.h"
#include "minzip/SysUtil.h"
#include "minzip/Zip.h"
#include "mtdutils/mounts.h"
#include "mtdutils/mtdutils.h"
#include "miui/src/miui.h"
}

#include "roots.h"
#include "verifier.h"
#include "root_device.hpp"


#define ASSUMED_UPDATE_BINARY_NAME  "META-INF/com/google/android/update-binary"
#define ASSUMED_UPDATE_SCRIPT_NAME  "META-INF/com/google/android/update-script"
#define PUBLIC_KEYS_FILE "/res/keys"

#define UPDATER_API_VERSION 3 // this should equal RECOVERY_API_VERSION , define in Android.mk 

// If the package contains an update binary, extract it and run it.
static int
try_update_binary(const char *path, ZipArchive *zip, int* wipe_cache) {
    const ZipEntry* binary_entry =
            mzFindZipEntry(zip, ASSUMED_UPDATE_BINARY_NAME);
    struct stat st;
    if (binary_entry == NULL) {
	    const ZipEntry* update_script_entry = 
		    mzFindZipEntry(zip, ASSUMED_UPDATE_SCRIPT_NAME);
	    if (update_script_entry != NULL) {
		     ui_print("Amend scripting (update-script) is no longer supported.\n");
            ui_print("Amend scripting was deprecated by Google in Android 1.5.\n");
            ui_print("It was necessary to remove it when upgrading to the ClockworkMod 3.0 Gingerbread based recovery.\n");
            ui_print("Please switch to Edify scripting (updater-script and update-binary) to create working update zip packages.\n");
            return INSTALL_ERROR;
        }

        mzCloseZipArchive(zip);
        return INSTALL_ERROR;
    }

    char* binary = "/tmp/update_binary";
    unlink(binary);
    int fd = creat(binary, 0755);
    if (fd < 0) {
        mzCloseZipArchive(zip);
        LOGE("Can't make %s\n", binary);
        return INSTALL_ERROR;
    }
    bool ok = mzExtractZipEntryToFile(zip, binary_entry, fd);
    close(fd);
    mzCloseZipArchive(zip);

    if (!ok) {
        LOGE("Can't copy %s\n", ASSUMED_UPDATE_BINARY_NAME);
	mzCloseZipArchive(zip);
        return INSTALL_ERROR;
    }


    //If exists, extract file_contexts from the zip file
    //Thanks twrp's Dees-Troy
    // begin -->
    const ZipEntry* selinx_contexts = mzFindZipEntry(zip, "file_contexts");
    if (selinx_contexts == NULL) {
	    mzCloseZipArchive(zip);
	    LOGI("Zip does not contain SElinux file_contexts file in its root.\n");
    } else {
	    char *output_filename = "/file_contexts";
	    LOGI("Zip contains SElinux file_contexts file in its root. Extracting to '%s'\n", output_filename);

	    //Delete any file_contexts
	    if (stat(output_filename,&st) == 0) 
		    unlink(output_filename);

	    int file_contexts_fd = creat(output_filename, 0644);
	    if (file_contexts_fd < 0) {
		    mzCloseZipArchive(zip);
		    LOGE("Could not extract file_contexts to '%s'\n", output_filename);
		    return INSTALL_ERROR;
	    }

	    ok = mzExtractZipEntryToFile(zip, selinx_contexts, file_contexts_fd);
    close(file_contexts_fd);

    if (!ok) {
	    mzCloseZipArchive(zip);
	    LOGE("Could not extract '%s'\n",ASSUMED_UPDATE_BINARY_NAME);
	    return INSTALL_ERROR;
    }
    mzCloseZipArchive(zip);
    }
    // <-- end 



    int pipefd[2];
    pipe(pipefd);
    char tmpbuf[256];

    // When executing the update binary contained in the package, the
    // arguments passed are:
    //
    //   - the version number for this interface
    //
    //   - an fd to which the program can write in order to update the
    //     progress bar.  The program can write single-line commands:
    //
    //        progress <frac> <secs>
    //            fill up the next <frac> part of of the progress bar
    //            over <secs> seconds.  If <secs> is zero, use
    //            set_progress commands to manually control the
    //            progress of this segment of the bar
    //
    //        set_progress <frac>
    //            <frac> should be between 0.0 and 1.0; sets the
    //            progress bar within the segment defined by the most
    //            recent progress command.
    //
    //        firmware <"hboot"|"radio"> <filename>
    //            arrange to install the contents of <filename> in the
    //            given partition on reboot.
    //
    //            (API v2: <filename> may start with "PACKAGE:" to
    //            indicate taking a file from the OTA package.)
    //
    //            (API v3: this command no longer exists.)
    //
    //        ui_print <string>
    //            display <string> on the screen.
    //
    //   - the name of the package zip file.
    //

    char** args = (char**)malloc(sizeof(char*) * 5);
    args[0] = binary;
    //args[1] = EXPAND(RECOVERY_API_VERSION);   // defined in Android.mk
    args[1] = (char*)EXPAND(UPDATER_API_VERSION);
    args[2] = (char*)malloc(10);
    sprintf(args[2], "%d", pipefd[1]);
    args[3] = (char*)path;
    args[4] = NULL;

    pid_t pid = fork();
    if (pid == 0) {
	setenv("UPDATE_PACKAGE", path, 1);
        close(pipefd[0]);
        execv(binary, args);
        //fprintf(stdout, "E:Can't run %s (%s)\n", binary, strerror(errno));
        _exit(-1);
    }
    close(pipefd[1]);

    *wipe_cache = 0;

    char buffer[1024];
    FILE* from_child = fdopen(pipefd[0], "r");
    while (fgets(buffer, sizeof(buffer), from_child) != NULL) {
        char* command = strtok(buffer, " \n");
        if (command == NULL) {
            continue;
        } else if (strcmp(command, "progress") == 0) {
            char* fraction_s = strtok(NULL, " \n");
            char* seconds_s = strtok(NULL, " \n");

            float fraction = strtof(fraction_s, NULL);
            int seconds = strtol(seconds_s, NULL, 10);

            ui_show_progress(fraction * (1-VERIFICATION_PROGRESS_FRACTION),
                             seconds);
        } else if (strcmp(command, "set_progress") == 0) {
            char* fraction_s = strtok(NULL, " \n");
            float fraction = strtof(fraction_s, NULL);
            ui_set_progress(fraction);
        } else if (strcmp(command, "ui_print") == 0) {
            char* str = strtok(NULL, "\n");
            if (str)
                snprintf(tmpbuf, 255, "<#selectbg_g><b>%s</b></#>", str);
            else 
                snprintf(tmpbuf, 255, "<#selectbg_g><b>\n</b></#>");
            miuiInstall_set_text(tmpbuf);
        } else if (strcmp(command, "wipe_cache") == 0) {
            *wipe_cache = 1;
        } else if (strcmp(command, "minzip:") == 0) {
            char* str = strtok(NULL, "\n");
            miuiInstall_set_info(str);
        } 
        else {
#if 0 
            snprintf(tmpbuf, 255, "%s", command);
	        miuiInstall_set_text(tmpbuf);
            char* str = strtok(NULL, "\n");
            if (str)
            {
                snprintf(tmpbuf, 255, "%s", str);
                miuiInstall_set_text(tmpbuf);
            }
#endif
            char * str = strtok(NULL, "\n");
            if (str)
                LOGD("[%s]:%s\n",command, str);
        }
    }
    fclose(from_child);

    int status;
    
    waitpid(pid, &status, 0);
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        LOGE("Error in %s\n(Status %d)\n", path, WEXITSTATUS(status));
        snprintf(tmpbuf, 255, "<#selectbg_g><b>Error in%s\n(Status %d)\n</b></#>", path, WEXITSTATUS(status));
	mzCloseZipArchive(zip);
        miuiInstall_set_text(tmpbuf);
        return INSTALL_ERROR;
    }
    mzCloseZipArchive(zip); 
    return INSTALL_SUCCESS;
}

// Reads a file containing one or more public keys as produced by
// DumpPublicKey:  this is an RSAPublicKey struct as it would appear
// as a C source literal, eg:
//
//  "{64,0xc926ad21,{1795090719,...,-695002876},{-857949815,...,1175080310}}"
//
// For key versions newer than the original 2048-bit e=3  keys
// Now is support 2048-bit e = 65537
// Supported by Android, the string is preceded by version 
// identifier, eg:
//  "v2 {64,0xc926ad21,{1795090719,...,-695002876},{-857949815,...,1175080310}}" 
//
//
// (Note that the braces and commas in this example are actual
// characters the parser expects to find in the file; the ellipses
// indicate more numbers omitted from this example.)
//
// The file may contain multiple keys in this format, separated by
// commas.  The last key must not be followed by a comma.
//
// Returns NULL if the file failed to parse, or if it contain zero keys.
static RSAPublicKey*
load_keys(const char* filename, int* numKeys) {
    RSAPublicKey* out = NULL;
    *numKeys = 0;

    FILE* f = fopen(filename, "r");
    if (f == NULL) {
        LOGE("opening %s: %s\n", filename, strerror(errno));
        goto exit;
    }
   {
    int i;
    bool done = false;
    while (!done) {
        ++*numKeys;
        out = (RSAPublicKey*)realloc(out, *numKeys * sizeof(RSAPublicKey));
        RSAPublicKey* key = out + (*numKeys - 1);
        if (fscanf(f, " { %i , 0x%x , { %u",
                   &(key->len), &(key->n0inv), &(key->n[0])) != 3) {
            goto exit;
        }
        if (key->len != RSANUMWORDS) {
            LOGE("key length (%d) does not match expected size\n", key->len);
            goto exit;
        }
        for (i = 1; i < key->len; ++i) {
            if (fscanf(f, " , %u", &(key->n[i])) != 1) goto exit;
        }
        if (fscanf(f, " } , { %u", &(key->rr[0])) != 1) goto exit;
        for (i = 1; i < key->len; ++i) {
            if (fscanf(f, " , %u", &(key->rr[i])) != 1) goto exit;
        }
        fscanf(f, " } } ");

        // if the line ends in a comma, this file has more keys.
        switch (fgetc(f)) {
            case ',':
                // more keys to come.
                break;

            case EOF:
                done = true;
                break;

            default:
                LOGE("unexpected character between keys\n");
                goto exit;
        }
    }
   }

    fclose(f);
    return out;

exit:
    if (f) fclose(f);
    free(out);
    *numKeys = 0;
    return NULL;
}

static int
really_install_package(const char *path, int* wipe_cache)
{
    ui_set_background(BACKGROUND_ICON_INSTALLING);
    ui_print("Finding update package...\n");
    ui_show_indeterminate_progress();
    LOGI("Update location: %s\n", path);

    if (ensure_path_mounted(path) != 0) {
        LOGE("Can't mount %s\n", path);
        return INSTALL_CORRUPT;
    }

    ui_print("Opening update package...\n");

    int err;
  /* Remove signature check to support ors */
    /*
    int sig_stat = 0;
     sig_stat = check_sig();
             if (sig_stat == -1) {
		     printf("skip Signature check ...\n");
	     }

    if (sig_stat == 1) {
	    int numkeys;
	    RSAPublicKey* loadedkeys = load_keys(PUBLIC_KEYS_FILE, &numkeys);
	   if (loadedkeys == NULL) {
		   LOGE("Failed to load keys\n");
		   return INSTALL_CORRUPT;
	   }
	   LOGI("%d key(s) loaded from %s\n", numkeys, PUBLIC_KEYS_FILE);

	   //Give verification half the progres bar...
	   ui_print("Verifying update package...\n");

	   err = verify_file(path, loadedkeys, numkeys);
	   free(loadedkeys);
	   LOGI("Verify_file returned %d\n", err);
	    if (err != VERIFY_SUCCESS) {
		    LOGE("Signature verification failed!\n");
			    return INSTALL_CORRUPT;
	    }
    }
*/

    /* Try to open the package.
     */
    ZipArchive zip;
    err = mzOpenZipArchive(path, &zip);
    if (err != 0) {
        LOGE("Can't open %s\n(%s)\n", path, err != -1 ? strerror(err) : "bad");
        return INSTALL_CORRUPT;
    }

    /* Verify and install the contents of the package.
     */
    ui_print("Installing update...\n");
    return try_update_binary(path, &zip, wipe_cache);
}

int
install_package(const char* path, int* wipe_cache, const char* install_file)
{
    FILE* install_log = fopen_path(install_file, "w");
    if (install_log) {
        fputs(path, install_log);
        fputc('\n', install_log);
    } else {
        LOGE("failed to open last_install: %s\n", strerror(errno));
    }
    int result = really_install_package(path, wipe_cache);
    if (install_log) {
        fputc(result == INSTALL_SUCCESS ? '1' : '0', install_log);
        fputc('\n', install_log);
        fclose(install_log);
    }
    return result;
}
