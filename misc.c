/*
 * nvidia-installer: A tool for installing NVIDIA software packages on
 * Unix and Linux systems.
 *
 * Copyright (C) 2003 NVIDIA Corporation
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms and conditions of the GNU General Public License,
 * version 2, as published by the Free Software Foundation.
 * 
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 * 
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, see <http://www.gnu.org/licenses>.
 *
 *
 * misc.c - this source file contains miscellaneous routines for use
 * by the nvidia-installer.
 */

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/utsname.h>
#include <sys/stat.h>
#include <ctype.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <dirent.h>
#include <libgen.h>
#include <pci/pci.h>

#ifndef PCI_CLASS_DISPLAY_3D
#define PCI_CLASS_DISPLAY_3D 0x302
#endif

#include "nvidia-installer.h"
#include "user-interface.h"
#include "kernel.h"
#include "files.h"
#include "misc.h"
#include "crc.h"
#include "nvLegacy.h"

static int check_symlink(Options*, const char*, const char*, const char*);
static int check_file(Options*, const char*, const mode_t, const uint32);


/*
 * read_next_word() - given a string buf, skip any whitespace, and
 * then copy the next set of characters until more white space is
 * encountered.  A new string containing this next word is returned.
 * The passed-by-reference parameter e, if not NULL, is set to point
 * at the where the end of the word was, to facilitate multiple calls
 * of read_next_word().
 */

char *read_next_word (char *buf, char **e)
{
    char *c = buf;
    char *start, *ret;
    int len;
    
    while ((*c) && (isspace (*c)) && (*c != '\n')) c++;
    start = c;
    while ((*c) && (!isspace (*c)) && (*c != '\n')) c++;
    
    len = c - start;

    if (len == 0) return NULL;
    
    ret = (char *) nvalloc (len + 1);

    strncpy (ret, start, len);
    ret[len] = '\0';

    if (e) *e = c;

    return ret;
    
} /* read_next_word() */



/*
 * check_euid() - this function checks that the effective uid of this
 * application is root, and calls the ui to print an error if it's not
 * root.
 */

int check_euid(Options *op)
{
    uid_t euid;

    euid = geteuid();
    
    if (euid != 0) {
        ui_error(op, "nvidia-installer must be run as root");
        return FALSE;
    }
    
    return TRUE;

} /* check_euid() */



/*
 * check_runlevel() - attempt to run the `runlevel` program.  If we
 * are in runlevel 1, explain why that is bad, and ask the user if
 * they want to continue anyway.
 */

int check_runlevel(Options *op)
{
    int ret;
    char *data, *cmd;
    char ignore, runlevel;

    if (op->no_runlevel_check) return TRUE;

    cmd = find_system_util("runlevel");
    if (!cmd) {
        ui_warn(op, "Skipping the runlevel check (the utility "
                "`runlevel` was not found)."); 
        return TRUE;
    }

    ret = run_command(op, cmd, &data, FALSE, FALSE, TRUE);
    nvfree(cmd);
    
    if ((ret != 0) || (!data)) {
        ui_warn(op, "Skipping the runlevel check (the utility "
                "`runlevel` failed to run)."); 
        return TRUE;
    }

    ret = sscanf(data, "%c %c", &ignore, &runlevel);

    if (ret != 2) {
        ui_warn(op, "Skipping the runlevel check (unrecognized output from "
                "the `runlevel` utility: '%d').", data);
        nvfree(data);
        return TRUE;
    }

    nvfree(data);

    if (runlevel == 's' || runlevel == 'S' || runlevel == '1') {
        ret = ui_yes_no(op, TRUE, "You appear to be running in runlevel 1; "
                        "this may cause problems.  For example: some "
                        "distributions that use devfs do not run the devfs "
                        "daemon in runlevel 1, making it difficult for "
                        "`nvidia-installer` to correctly setup the kernel "
                        "module configuration files.  It is recommended "
                        "that you quit installation now and switch to "
                        "runlevel 3 (`telinit 3`) before installing.\n\n"
                        "Quit installation now? (select 'No' to continue "
                        "installation)");
        
        if (ret) return FALSE;
    }

    return TRUE;

} /* check_runlevel() */



/* 
 * adjust_cwd() - this function scans through program_name (ie
 * argv[0]) for any possible relative paths, and chdirs into the
 * relative path it finds.  The point of all this is to make the
 * directory with the executed binary the cwd.
 *
 * It is assumed that the user interface has not yet been initialized
 * by the time this function is called.
 */

int adjust_cwd(Options *op, const char *program_name)
{
    char *c, *path;
    int len;
    
    /*
     * extract any pathname portion out of the program_name and chdir
     * to it
     */
    
    c = strrchr(program_name, '/');
    if (c) {
        len = c - program_name + 1;
        path = (char *) nvalloc(len + 1);
        strncpy(path, program_name, len);
        path[len] = '\0';
        if (op->expert) log_printf(op, TRUE, NULL, "chdir(\"%s\")", path);
        if (chdir(path)) {
            fprintf(stderr, "Unable to chdir to %s (%s)",
                    path, strerror(errno));
            return FALSE;
        }
        free(path);
    }
    
    return TRUE;
    
} /* adjust_cwd() */


/*
 * get_next_line() - this function scans for the next newline,
 * carriage return, NUL terminator, or EOF in buf.  If non-NULL, the
 * passed-by-reference parameter 'end' is set to point to the next
 * printable character in the buffer, or NULL if EOF is encountered.
 *
 * If the parameter 'start' is non-NULL, then that is interpretted as
 * the start of the buffer string, and we check that we never walk
 * 'length' bytes past 'start'.
 * 
 * On success, a newly allocated buffer is allocated containing the
 * next line of text (with a NULL terminator in place of the
 * newline/carriage return).
 *
 * On error, NULL is returned.
 */

char *get_next_line(char *buf, char **end, char *start, int length)
{
    char *c, *retbuf;
    int len;

    if (start && (length < 1)) return NULL;

#define __AT_END(_start, _current, _length) \
    ((_start) && (((_current) - (_start)) >= (_length)))
    
    if (end) *end = NULL;
    
    // Cast all char comparisons to EOF to signed char in order to
    // allow proper sign extension on platforms like GCC ARM where
    // char is unsigned char
    if ((!buf) ||
        __AT_END(start, buf, length) ||
        (*buf == '\0') ||
        (((signed char)*buf) == EOF)) return NULL;
    
    c = buf;
    
    while ((!__AT_END(start, c, length)) &&
           (*c != '\0') &&
           (((signed char)*c) != EOF) &&
           (*c != '\n') &&
           (*c != '\r')) c++;

    len = c - buf;
    retbuf = nvalloc(len + 1);
    strncpy(retbuf, buf, len);
    retbuf[len] = '\0';
    
    if (end) {
        while ((!__AT_END(start, c, length)) &&
               (*c != '\0') &&
               (((signed char)*c) != EOF) &&
               (!isprint(*c))) c++;
        
        if (__AT_END(start, c, length) ||
            (*c == '\0') ||
            (((signed char)*c) == EOF)) *end = NULL;
        else *end = c;
    }
    
    return retbuf;

} /* get_next_line() */



/*
 * run_command() - this function runs the given command and assigns
 * the data parameter to a malloced buffer containing the command's
 * output, if any.  The caller of this function should free the data
 * string.  The return value of the command is returned from this
 * function.
 *
 * The output parameter controls whether command output is sent to the
 * ui; if this is TRUE, then everyline of output that is read is sent
 * to the ui.
 *
 * If the status parameter is greater than 0, it is interpretted as a
 * rough estimate of how many lines of output will be generated by the
 * command.  This is used to compute the value that should be passed
 * to ui_status_update() for every line of output that is received.
 *
 * The redirect argument tells run_command() to redirect stderr to
 * stdout so that all output is collected, or just stdout.
 *
 * XXX maybe we should do something to cap the time we allow the
 * command to run?
 */

int run_command(Options *op, const char *cmd, char **data, int output,
                int status, int redirect)
{
    int n, len, buflen, ret;
    char *cmd2, *buf, *tmpbuf;
    FILE *stream = NULL;
    struct sigaction act, old_act;
    float percent;
    
    if (data) *data = NULL;

    /*
     * if command output is requested, print the command that we will
     * execute
     */

    if (output) ui_command_output (op, "executing: '%s'...", cmd);

    /* redirect stderr to stdout */

    if (redirect) {
        cmd2 = nvstrcat(cmd, " 2>&1", NULL);
    } else {
        cmd2 = nvstrdup(cmd);
    }
    
    /*
     * XXX: temporarily ignore SIGWINCH; our child process inherits
     * this disposition and will likewise ignore it (by default).
     * This fixes cases where child processes abort after receiving
     * SIGWINCH when its caught in the parent process.
     */
    if (op->sigwinch_workaround) {
        act.sa_handler = SIG_IGN;
        sigemptyset(&act.sa_mask);
        act.sa_flags = 0;

        if (sigaction(SIGWINCH, &act, &old_act) < 0)
            old_act.sa_handler = NULL;
    }

    /*
     * Open a process by creating a pipe, forking, and invoking the
     * command.
     */
    
    if ((stream = popen(cmd2, "r")) == NULL) {
        ui_error(op, "Failure executing command '%s' (%s).",
                 cmd, strerror(errno));
        return errno;
    }
    
    free(cmd2);

    /*
     * read from the stream, filling and growing buf, until we hit
     * EOF.  Send each line to the ui as it is read.
     */
    
    len = 0;    /* length of what has actually been read */
    buflen = 0; /* length of destination buffer */
    buf = NULL;
    n = 0;      /* output line counter */

    while (1) {
        
        if ((buflen - len) < NV_MIN_LINE_LEN) {
            buflen += NV_LINE_LEN;
            tmpbuf = (char *) nvalloc(buflen);
            if (buf) {
                memcpy(tmpbuf, buf, len);
                free(buf);
            }
            buf = tmpbuf;
        }
        
        if (fgets(buf + len, buflen - len, stream) == NULL) break;
        
        if (output) ui_command_output(op, "%s", buf + len);
        
        len += strlen(buf + len);

        if (status) {
            n++;
            if (n > status) n = status;
            percent = (float) n / (float) status;

            /*
             * XXX: manually call the SIGWINCH handler, if set, to
             * handle window resizes while we ignore the signal.
             */
            if (op->sigwinch_workaround)
                if (old_act.sa_handler) old_act.sa_handler(SIGWINCH);

            ui_status_update(op, percent, NULL);
        }
    } /* while (1) */

    /* Close the popen()'ed stream. */

    ret = pclose(stream);

    /*
     * Restore the SIGWINCH signal disposition and handler, if any,
     * to their original values.
     */
    if (op->sigwinch_workaround)
        sigaction(SIGWINCH, &old_act, NULL);

    /* if the last character in the buffer is a newline, null it */
    
    if ((len > 0) && (buf[len-1] == '\n')) buf[len-1] = '\0';
    
    if (data) *data = buf;
    else free(buf);
    
    return ret;
    
} /* run_command() */



/*
 * read_text_file() - open a text file, read its contents and return
 * them to the caller in a newly allocated buffer.  Returns TRUE on
 * success and FALSE on failure.
 */

int read_text_file(const char *filename, char **buf)
{
    FILE *fp;
    int index = 0, buflen = 0;
    int eof = FALSE;
    char *line, *tmpbuf;

    *buf = NULL;

    fp = fopen(filename, "r");
    if (!fp)
        return FALSE;

    while (((line = fget_next_line(fp, &eof))
                != NULL) && !eof) {
        if ((index + strlen(line) + 1) > buflen) {
            buflen += 2 * strlen(line);
            tmpbuf = (char *)nvalloc(buflen);
            if (!tmpbuf) {
                if (*buf) nvfree(*buf);
                fclose(fp);
                return FALSE;
            }
            if (*buf) {
                memcpy(tmpbuf, *buf, index);
                nvfree(*buf);
            }
            *buf = tmpbuf;
        }

        index += sprintf(*buf + index, "%s\n", line);
        nvfree(line);
    }

    fclose(fp);

    return TRUE;

} /* read_text_file() */



/*
 * find_system_utils() - search the $PATH (as well as some common
 * additional directories) for the utilities that the installer will
 * need to use.  Returns TRUE on success and assigns the util fields
 * in the option struct; it returns FALSE on failure.
 *
 * XXX requiring ld may cause problems
 */

#define EXTRA_PATH "/bin:/usr/bin:/sbin:/usr/sbin:/usr/X11R6/bin:/usr/bin/X11"

int find_system_utils(Options *op)
{
    /* keep in sync with the SystemUtils enum type */
    const struct { const char *util, *package; } needed_utils[] = {
        { "ldconfig", "glibc" },
        { "ldd",      "glibc" },
        { "ld",       "binutils" },
        { "objcopy",  "binutils" },
        { "grep",     "grep" },
        { "dmesg",    "util-linux" },
        { "tail",     "coreutils" },
        { "cut",      "coreutils" },
        { "tr",       "coreutils" },
        { "sed",      "sed" }
    };
    
    /* keep in sync with the SystemOptionalUtils enum type */
    const struct { const char *util, *package; } optional_utils[] = {
        { "chcon",          "selinux" },
        { "selinuxenabled", "selinux" },
        { "getenforce",     "selinux" },
        { "execstack",      "selinux" },
        { "pkg-config",     "pkg-config" },
        { "X",              "xserver" }
    };
    
    int i, j;

    ui_expert(op, "Searching for system utilities:");

    /* search the PATH for each utility */

    for (i = 0; i < MAX_SYSTEM_UTILS; i++) {
        op->utils[i] = find_system_util(needed_utils[i].util);
        if (!op->utils[i]) {
            ui_error(op, "Unable to find the system utility `%s`; please "
                     "make sure you have the package '%s' installed.  If "
                     "you do have %s installed, then please check that "
                     "`%s` is in your PATH.",
                     needed_utils[i].util, needed_utils[i].package,
                     needed_utils[i].package, needed_utils[i].util);
            return FALSE;
        }
        
        ui_expert(op, "found `%s` : `%s`",
                  needed_utils[i].util, op->utils[i]);
    }
    
    for (j = 0, i = MAX_SYSTEM_UTILS; i < MAX_SYSTEM_OPTIONAL_UTILS; i++, j++) {
        
        op->utils[i] = find_system_util(optional_utils[j].util);
        if (op->utils[i]) {     
            ui_expert(op, "found `%s` : `%s`",
                  optional_utils[j].util, op->utils[i]);
        } else {
            op->utils[i] = NULL;
        }
    }
    
    return TRUE;

} /* find_system_utils() */



typedef struct {
    const char *util;
    const char *package;
} Util;

/* keep in sync with the ModuleUtils enum type */
static Util __module_utils[] = {
    { "insmod",   "module-init-tools" },
    { "modprobe", "module-init-tools" },
    { "rmmod",    "module-init-tools" },
    { "lsmod",    "module-init-tools" },
    { "depmod",   "module-init-tools" },
};

/* keep in sync with the ModuleUtils enum type */
static Util __module_utils_linux24[] = {
    { "insmod",   "modutils" },
    { "modprobe", "modutils" },
    { "rmmod",    "modutils" },
    { "lsmod",    "modutils" },
    { "depmod",   "modutils" },
};

/*
 * find_module_utils() - search the $PATH (as well as some common
 * additional directories) for the utilities that the installer will
 * need to use.  Returns TRUE on success and assigns the util fields
 * in the option struct; it returns FALSE on failures.
 */

int find_module_utils(Options *op)
{
    int i, j;
    Util *needed_utils = __module_utils;

    if (strncmp(get_kernel_name(op), "2.4", 3) == 0)
        needed_utils = __module_utils_linux24;

    ui_expert(op, "Searching for module utilities:");

    /* search the PATH for each utility */

    for (j=0, i = MAX_SYSTEM_OPTIONAL_UTILS; i < MAX_UTILS; i++, j++) {
        op->utils[i] = find_system_util(needed_utils[j].util);
        if (!op->utils[i]) {
            ui_error(op, "Unable to find the module utility `%s`; please "
                     "make sure you have the package '%s' installed.  If "
                     "you do have %s installed, then please check that "
                     "`%s` is in your PATH.",
                     needed_utils[j].util, needed_utils[j].package,
                     needed_utils[j].package, needed_utils[j].util);
            return FALSE;
        }

        ui_expert(op, "found `%s` : `%s`",
                  needed_utils[j].util, op->utils[i]);
    };

    return TRUE;

} /* find_module_utils() */


/*
 * check_proc_modprobe_path() - check if the modprobe path reported
 * via /proc matches the one determined earlier; also check if it can
 * be accessed/executed.
 */

#define PROC_MODPROBE_PATH_FILE "/proc/sys/kernel/modprobe"

int check_proc_modprobe_path(Options *op)
{
    FILE *fp;
    char *buf = NULL;

    fp = fopen(PROC_MODPROBE_PATH_FILE, "r");
    if (fp) {
        buf = fget_next_line(fp, NULL);
        fclose(fp);
    }

    if (buf && strcmp(buf, op->utils[MODPROBE])) {
        if (access(buf, F_OK | X_OK) == 0) {
            ui_warn(op, "The path to the `modprobe` utility reported by "
                    "'%s', `%s`, differs from the path determined by "
                    "`nvidia-installer`, `%s`.  Please verify that `%s` "
                    "works correctly and correct the path in '%s' if "
                    "it does not.",
                    PROC_MODPROBE_PATH_FILE, buf, op->utils[MODPROBE],
                    buf, PROC_MODPROBE_PATH_FILE);
            return TRUE;
        } else {
           ui_error(op, "The path to the `modprobe` utility reported by "
                    "'%s', `%s`, differs from the path determined by "
                    "`nvidia-installer`, `%s`, and does not appear to "
                    "point to a valid `modprobe` binary.  Please correct "
                    "the path in '%s'.",
                    PROC_MODPROBE_PATH_FILE, buf, op->utils[MODPROBE],
                    PROC_MODPROBE_PATH_FILE);
           return FALSE;
        }
    } else if (!buf && strcmp("/sbin/modprobe", op->utils[MODPROBE])) {
        if (access(buf, F_OK | X_OK) == 0) {
            ui_warn(op, "The file '%s' is unavailable, the X server will "
                    "use `/sbin/modprobe` as the path to the `modprobe` "
                    "utility.  This path differs from the one determined "
                    "by `nvidia-installer`, `%s`.  Please verify that "
                    "`/sbin/modprobe` works correctly or mount the /proc "
                    "file system and verify that '%s' reports the "
                    "correct path.",
                    PROC_MODPROBE_PATH_FILE, op->utils[MODPROBE],
                    PROC_MODPROBE_PATH_FILE);
            return TRUE;
        } else {
           ui_error(op, "The file '%s' is unavailable, the X server will "
                    "use `/sbin/modprobe` as the path to the `modprobe` "
                    "utility.  This path differs from the one determined "
                    "by `nvidia-installer`, `%s`, and does not appear to "
                    "point to a valid `modprobe` binary.  Please create "
                    "a symbolic link from `/sbin/modprobe` to `%s` or "
                    "mount the /proc file system and verify that '%s' "
                    "reports the correct path.",
                    PROC_MODPROBE_PATH_FILE, op->utils[MODPROBE],
                    op->utils[MODPROBE], PROC_MODPROBE_PATH_FILE);
           return FALSE;
        }
    }
    nvfree(buf);

    return TRUE;

} /* check_proc_modprobe_path() */


/*
 * check_development_tools() - check if the development tools needed
 * to build custom kernel interfaces are available.
 */

int check_development_tools(Options *op, Package *p)
{
#define MAX_TOOLS 2
    const struct { char *tool, *package; } needed_tools[] = {
        { "cc",   "gcc"  },
        { "make", "make" }
    };

    int i, ret;
    char *tool, *cmd, *CC, *result;

    CC = getenv("CC");

    ui_expert(op, "Checking development tools:");

    /*
     * Check if the required toolchain components are installed on
     * the system.  Note that we skip the check for `cc` if the
     * user specified the CC environment variable; we do this because
     * `cc` may not be present in the path, nor the compiler named
     * in $CC, but the installation may still succeed. $CC is sanity
     * checked below.
     */

    for (i = (CC != NULL) ? 1 : 0; i < MAX_TOOLS; i++) {
        tool = find_system_util(needed_tools[i].tool);
        if (!tool) {
            ui_error(op, "Unable to find the development tool `%s` in "
                     "your path; please make sure that you have the "
                     "package '%s' installed.  If %s is installed on your "
                     "system, then please check that `%s` is in your "
                     "PATH.",
                     needed_tools[i].tool, needed_tools[i].package,
                     needed_tools[i].package, needed_tools[i].tool);
            return FALSE;
        }

        ui_expert(op, "found `%s` : `%s`", needed_tools[i].tool, tool);
    }

    /*
     * Check if the libc development headers are installed; we need
     * these to build the CC version check utility.
     */
    if (access("/usr/include/stdio.h", F_OK) == -1) {
        ui_error(op, "You do not appear to have libc header files "
                 "installed on your system.  Please install your "
                 "distribution's libc development package.");
        return FALSE;
    }

    if (!CC) CC = "cc";

    ui_log(op, "Performing CC sanity check with CC=\"%s\".", CC);

    cmd = nvstrcat("sh ", p->kernel_module_build_directory,
                   "/conftest.sh ", CC, " ", CC, " ",
                   "DUMMY_SOURCE DUMMY_OUTPUT ",
                   "cc_sanity_check just_msg", NULL);

    ret = run_command(op, cmd, &result, FALSE, 0, TRUE);

    nvfree(cmd);

    if (ret == 0) return TRUE;

    ui_error(op, "The CC sanity check failed:\n\n%s\n", result);

    nvfree(result);

    return FALSE;

} /* check_development_tools() */


/*
 * find_system_util() - build a search path and search for the named
 * utility.  If the utility is found, the fully qualified path to the
 * utility is returned.  On failure NULL is returned.
 */

char *find_system_util(const char *util)
{
    char *buf, *path, *file, *x, *y, c;
    
    /* build the search path */
    
    buf = getenv("PATH");
    if (buf) {
        path = nvstrcat(buf, ":", EXTRA_PATH, NULL);
    } else {
        path = nvstrdup(EXTRA_PATH);
    }

    /* search the PATH for the utility */

    for (x = y = path; ; x++) {
        if (*x == ':' || *x == '\0') {
            c = *x;
            *x = '\0';
            file = nvstrcat(y, "/", util, NULL);
            *x = c;
            if ((access(file, F_OK | X_OK)) == 0) {
                nvfree(path);
                return file;
            }
            nvfree(file);
            y = x + 1;
            if (*x == '\0') break;
        }
    }

    nvfree(path);

    return NULL;

} /* find_system_util() */



/*
 * continue_after_error() - tell the user that an error has occured,
 * and ask them if they would like to continue.
 *
 * Returns TRUE if the installer should continue.
 */

int continue_after_error(Options *op, const char *fmt, ...)
{
    char *msg;
    int ret;

    NV_VSNPRINTF(msg, fmt);
    
    ret = ui_yes_no(op, TRUE, "The installer has encountered the following "
                    "error during installation: '%s'.  Continue anyway? "
                    "(\"no\" will abort)?", msg);

    nvfree(msg);

    return ret;

} /* continue_after_error() */



/*
 * do_install()
 */

int do_install(Options *op, Package *p, CommandList *c)
{
    char *msg;
    int len, ret;

    len = strlen(p->description) + strlen(p->version) + 64;
    msg = (char *) nvalloc(len);
    snprintf(msg, len, "Installing '%s' (%s):",
             p->description, p->version);
    
    ret = execute_command_list(op, c, msg, "Installing");
    
    free(msg);
    
    if (!ret) return FALSE;
    
    ui_log(op, "Driver file installation is complete.", p->description);

    return TRUE;

} /* do_install() */



/*
 * extract_version_string() - extract the NVIDIA driver version string
 * from the given string.  On failure, return NULL; on success, return
 * a malloced string containing just the version string.
 *
 * The version string can have one of two forms: either the old
 * "X.Y.ZZZZ" format (e.g., "1.0-9742"), or the new format where it is
 * just a collection of period-separated numbers (e.g., "105.17.2").
 * The length and number of periods in the newer format is arbitrary.
 *
 * Furthermore, we expect the new version format to be enclosed either
 * in parenthesis or whitespace (or be at the start or end of the
 * input string) and be atleast 5 characters long.  This allows us to
 * distinguish the version string from other numbers such as the year
 * or the old version format in input strings like this:
 *
 *  "NVIDIA UNIX x86 Kernel Module  105.17.2  Fri Dec 15 09:54:45 PST 2006"
 *  "1.0-105917 (105.9.17)"
 */

char *extract_version_string(const char *str)
{
    char c, *copiedString, *start, *end, *x, *version = NULL;
    int state;

    if (!str) return NULL;

    copiedString = strdup(str);
    x = copiedString;
    
    /*
     * look for a block of only numbers and periods; the version
     * string must be surrounded by either whitespace, or the
     * start/end of the string; we use a small state machine to parse
     * the string
     */
    
    start = NULL;
    end = NULL;

#define STATE_IN_VERSION          0
#define STATE_NOT_IN_VERSION      1
#define STATE_LOOKING_FOR_VERSION 2
#define STATE_FOUND_VERSION       3

    state = STATE_LOOKING_FOR_VERSION;

    while (*x) {
        
        c = *x;
        
        switch (state) {
        
            /*
             * if we are LOOKING_FOR_VERSION, then finding a digit
             * will put us inside the version, whitespace (or open
             * parenthesis) will allow us to continue to look for the
             * version, and any other character will cause us to stop
             * looking for the version string
             */
    
        case STATE_LOOKING_FOR_VERSION:
            if (isdigit(c)) {
                start = x;
                state = STATE_IN_VERSION;
            } else if (isspace(c) || (c == '(')) {
                state = STATE_LOOKING_FOR_VERSION;
            } else {
                state = STATE_NOT_IN_VERSION;
            }
            break;
            
            /*
             * if we are IN_VERSION, then a digit or period will keep
             * us in the version, space (or close parenthesis) and
             * more than 4 characters of version means we found the
             * entire version string.  If we find any other character,
             * then what we thought was the version string wasn't, so
             * move to NOT_IN_VERSION.
             */

        case STATE_IN_VERSION:
            if (isdigit(c) || (c == '.')) {
                state = STATE_IN_VERSION;
            } else if ((isspace(c) || (c == ')')) && ((x - start) >= 5)) {
                end = x;
                state = STATE_FOUND_VERSION;
                goto exit_while_loop;
            } else {
                state = STATE_NOT_IN_VERSION;
            }
            break;
            
            /*
             * if we are NOT_IN_VERSION, then space or open
             * parenthesis will make us start looking for the version,
             * and any other character just leaves us in the
             * NOT_IN_VERSION state
             */

        case STATE_NOT_IN_VERSION:
            if (isspace(c) || (c == '(')) {
                state = STATE_LOOKING_FOR_VERSION;
            } else {
                state = STATE_NOT_IN_VERSION;
            }
            break;
        }

        x++;
    }

    /*
     * the NULL terminator that broke us out of the while loop could
     * be the end of the version string
     */
    
    if ((state == STATE_IN_VERSION) && ((x - start) >= 5)) {
        end = x;
        state = STATE_FOUND_VERSION;
    }
    
 exit_while_loop:
    
    /* if we found a version string above, copy it */

    if (state == STATE_FOUND_VERSION) {
        *end = '\0';
        version = strdup(start);
        goto done;
    }
    
    
    
    /*
     * we did not find a version string with the new format; look for
     * a version of the old X.Y-ZZZZ format
     */
    
    x = copiedString;

    while (*x) {
        if (((x[0]) && isdigit(x[0])) &&
            ((x[1]) && (x[1] == '.')) &&
            ((x[2]) && isdigit(x[2])) &&
            ((x[3]) && (x[3] == '-')) &&
            ((x[4]) && isdigit(x[4])) &&
            ((x[5]) && isdigit(x[5])) &&
            ((x[6]) && isdigit(x[6])) &&
            ((x[7]) && isdigit(x[7]))) {
            
            x[8] = '\0';
            
            version = strdup(x);
            goto done;
        }
        x++;
    }

 done:

    free(copiedString);

    return version;

} /* extract_version_string() */



/*
 * should_install_opengl_headers() - if in expert mode, ask the user
 * if they want to install OpenGL header files.
 */

void should_install_opengl_headers(Options *op, Package *p)
{
    int i, have_headers = FALSE;

    if (!op->expert) return;

    /*
     * first, scan through the package to see if we have any header
     * files to install
     */

    for (i = 0; i < p->num_entries; i++) {
        if (p->entries[i].flags & FILE_TYPE_OPENGL_HEADER) {
            have_headers = TRUE;
            break;
        }
    }

    if (!have_headers) return;

    /*
     * If we're to provide more verbose descriptions, we could present
     * something like this:
     *
     * ("The %s provides OpenGL header files; these are used when
     * compiling OpenGL applications.  Most Linux distributions
     * already have OpenGL header files installed (normally in the
     * /usr/include/GL/ directory).  If you don't have OpenGL header
     * files installed and would like to, or if you want to develop
     * OpenGL applications that take advantage of NVIDIA OpenGL
     * extensions, then you can install NVIDIA's OpenGL header files
     * at this time.", p->description);
     */

    op->opengl_headers = ui_yes_no(op, op->opengl_headers,
                                   "Install NVIDIA's OpenGL header files?");

    ui_expert(op, "Installation %s install the OpenGL header files.",
              op->opengl_headers ? "will" : "will not");

} /* should_install_opengl_headers() */



/*
 * should_install_compat32_files() - ask the user if he/she wishes to
 * install 32bit compatibily libraries.
 */

void should_install_compat32_files(Options *op, Package *p)
{
#if defined(NV_X86_64)
    int i, have_compat32_files = FALSE, install_compat32_files;

    /*
     * first, scan through the package to see if we have any
     * 32bit compatibility files to install.
     */

    for (i = 0; i < p->num_entries; i++) {
        if (p->entries[i].flags & FILE_CLASS_COMPAT32) {
            have_compat32_files = TRUE;
            break;
        }
    }

    if (!have_compat32_files)
        return;

    /*
     * Ask the user if the 32-bit compatibility libraries are
     * to be installed. If yes, check if the chosen prefix
     * exists. If not, notify the user and ask him/her if the
     * files are to be installed anyway.
     */
    install_compat32_files = ui_yes_no(op, TRUE,
                "Install NVIDIA's 32-bit compatibility OpenGL "
                "libraries?");

    if (install_compat32_files && (op->compat32_chroot != NULL) &&
          access(op->compat32_chroot, F_OK) < 0) {
        install_compat32_files = ui_yes_no(op, FALSE,
            "The NVIDIA 32-bit compatibility OpenGL libraries are "
            "to be installed relative to the top-level prefix (chroot) "
            "'%s'; however, this directory does not exist.  Please "
            "consult your distribution's documentation to confirm the "
            "correct top-level installation prefix for 32-bit "
            "compatiblity libraries.\n\nDo you wish to install the "
            "32-bit NVIDIA OpenGL compatibility libraries anyway?",
            op->compat32_chroot);
    }
    
    if (!install_compat32_files) {
        for (i = 0; i < p->num_entries; i++) {
            if (p->entries[i].flags & FILE_CLASS_COMPAT32) {
                /* invalidate file */
                p->entries[i].flags &= ~FILE_TYPE_MASK;
                p->entries[i].dst = NULL;
            }
        }
    }
#endif /* NV_X86_64 */
}


/*
 * check_installed_files_from_package() - scan through the entries in
 * the package, making sure that all symbolic links and files are
 * properly installed.
 */

void check_installed_files_from_package(Options *op, Package *p)
{
    int i, ret = TRUE;
    float percent;
    uint64_t installable_files;

    ui_status_begin(op, "Running post-install sanity check:", "Checking");

    installable_files = get_installable_file_mask(op);
    
    for (i = 0; i < p->num_entries; i++) {
        
        percent = (float) i / (float) p->num_entries;
        ui_status_update(op, percent, p->entries[i].dst);
        
        if (p->entries[i].flags & FILE_TYPE_SYMLINK) {
            /* Don't bother checking FILE_TYPE_NEWSYMs because we may not have
             * installed them. */
            if (!check_symlink(op, p->entries[i].target,
                               p->entries[i].dst,
                               p->description)) {
                ret = FALSE;
            }
        } else if (p->entries[i].flags & installable_files) {
            if (!check_file(op, p->entries[i].dst, p->entries[i].mode, 0)) {
                ret = FALSE;
            }
        }
    }

    ui_status_end(op, "done.");
    ui_log(op, "Post-install sanity check %s.", ret ? "passed" : "failed");

} /* check_installed_files_from_package() */



/*
 * check_symlink() - check that the specified symbolic link exists and
 * point to the correct target.  Print descriptive warnings if
 * anything about the symbolic link doesn't appear as it should.
 *
 * Returns FALSE if the symbolic link appeared wrong; returns TRUE if
 * everything appears in order.
 */

static int check_symlink(Options *op, const char *target, const char *link,
                         const char *descr)
{
    char *actual_target;

    actual_target = get_symlink_target(op, link);
    if (!actual_target) {
        ui_warn(op, "The symbolic link '%s' does not exist.  This is "
                "necessary for correct operation of the %s.  You can "
                "create this symbolic link manually by executing "
                "`ln -sf %s %s`.",
                link,
                descr,
                target,
                link);
        return FALSE;
    } 

    if (strcmp(actual_target, target) != 0) {
        ui_warn(op, "The symbolic link '%s' does not point to '%s' "
                "as is necessary for correct operation of the %s.  "
                "It is possible that `ldconfig` has created this "
                "incorrect symbolic link because %s's "
                "\"soname\" conflicts with that of %s.  It is "
                "recommended that you remove or rename the file "
                "'%s' and create the necessary symbolic link by "
                "running `ln -sf %s %s`.",
                link,
                target,
                descr,
                actual_target,
                target,
                actual_target,
                target,
                link);
        free(actual_target);
        return FALSE;
    }
    return TRUE;
    
} /* check_symlink() */



/*
 * check_file() - check that the specified installed file exists, has
 * the correct permissions, and has the correct crc.
 *
 * If anything is incorrect, print a warning and return FALSE,
 * otherwise return TRUE.
 */

static int check_file(Options *op, const char *filename,
                      const mode_t mode, const uint32 crc)
{
    struct stat stat_buf;
    uint32 actual_crc;

    if (lstat(filename, &stat_buf) == -1) {
        ui_warn(op, "Unable to find installed file '%s' (%s).",
                filename, strerror(errno));
        return FALSE;
    }

    if (!S_ISREG(stat_buf.st_mode)) {
        ui_warn(op, "The installed file '%s' is not of the correct filetype.",
                filename);
        return FALSE;
    }

    if ((stat_buf.st_mode & PERM_MASK) != (mode & PERM_MASK)) {
        ui_warn(op, "The installed file '%s' has permissions %04o, but it "
                "was installed with permissions %04o.", filename,
                (stat_buf.st_mode & PERM_MASK),
                (mode & PERM_MASK));
        return FALSE;
    }

    /* only check the crc if we were handed a non-emtpy crc */

    if (crc != 0) {
        actual_crc = compute_crc(op, filename);
        if (crc != actual_crc) {
            ui_warn(op, "The installed file '%s' has a different checksum "
                    "(%ul) than when it was installed (%ul).",
                    filename, actual_crc, crc);
            return FALSE;
        }
    }

    return TRUE;
    
} /* check_file() */



/*
 * get_installable_file_mask() - return the mask of what file types
 * should be considered installable.
 */

uint64_t get_installable_file_mask(Options *op)
{
    uint64_t installable_files = FILE_TYPE_INSTALLABLE_FILE;
    if (!op->opengl_headers) installable_files &= ~FILE_TYPE_OPENGL_HEADER;
    if (op->no_kernel_module_source)
        installable_files &= ~FILE_TYPE_KERNEL_MODULE_SRC;
    if (!op->xorg_supports_output_class)
        installable_files &= ~FILE_TYPE_XORG_OUTPUTCLASS_CONFIG;
    return installable_files;

} /* get_installable_file_mask() */



#if defined(NV_TLS_TEST)
/*
 * tls_test() - Starting with glibc 2.3, there is a new thread local
 * storage mechanism.  To accomodate this, NVIDIA's OpenGL libraries
 * are built both the "classic" way, and the new way.  To determine
 * which set of OpenGL libraries to install, execute the test program
 * stored in tls_test_array.  If the program returns 0 we should
 * install the new tls libraries; if it returns anything else, we
 * should install the "classic" libraries.
 *
 * So as to avoid any risk of not being able to find the tls_test
 * binary at run time, the test program is stored as static data
 * inside the installer binary (in the same way that the user
 * interface shared libraries are)... see
 * user_interface.c:extract_user_interface() for details.
 *
 * Return TRUE if the new tls libraries should be installed; FALSE if
 * the old libraries should be used.
 */

/* pull in the array and size from g_tls_test.c */

extern const unsigned char tls_test_array[];
extern const int tls_test_array_size;

/* pull in the array and size from g_tls_test_dso.c */

extern const unsigned char tls_test_dso_array[];
extern const int tls_test_dso_array_size;



#if defined(NV_X86_64)

/* pull in the array and size from g_tls_test_32.c */

extern const unsigned char tls_test_array_32[];
extern const int tls_test_array_32_size;

/* pull in the array and size from g_tls_test_dso_32.c */

extern const unsigned char tls_test_dso_array_32[];
extern const int tls_test_dso_array_32_size;

#endif /* NV_X86_64 */


/* forward prototype */

static int tls_test_internal(Options *op, int which_tls,
                             const unsigned char *test_array,
                             const int test_array_size,
                             const unsigned char *dso_test_array,
                             const int dso_test_array_size);



int tls_test(Options *op, int compat_32_libs)
{
    if (compat_32_libs) {
        
#if defined(NV_X86_64)
        return tls_test_internal(op, op->which_tls_compat32,
                                 tls_test_array_32,
                                 tls_test_array_32_size,
                                 tls_test_dso_array_32,
                                 tls_test_dso_array_32_size);
#else
        return FALSE;
#endif /* NV_X86_64 */        
        
    } else {
        return tls_test_internal(op, op->which_tls,
                                 tls_test_array,
                                 tls_test_array_size,
                                 tls_test_dso_array,
                                 tls_test_dso_array_size);
    }
} /* tls_test */



/*
 * tls_test_internal() - this is the routine that does all the work to
 * write the tests to file and execute them; the caller (tls_test())
 * just selects which array data is used as the test.
 */

static int tls_test_internal(Options *op, int which_tls,
                             const unsigned char *test_array,
                             const int test_array_size,
                             const unsigned char *test_dso_array,
                             const int test_dso_array_size)
{
    int ret = FALSE;
    char *tmpfile = NULL, *dso_tmpfile = NULL, *cmd = NULL;
    
    /* allow commandline options to bypass this test */
    
    if (which_tls == FORCE_NEW_TLS) return TRUE;
    if (which_tls == FORCE_CLASSIC_TLS) return FALSE;
    
    /* check that we have the test program */

    if ((test_array == NULL) ||
        (test_array_size == 0) ||
        (test_dso_array == NULL) ||
        (test_dso_array_size == 0)) {
        ui_warn(op, "The thread local storage test program is not "
                "present; assuming classic tls.");
        return FALSE;
    }
    
    /* write the tls_test data to tmp files */
    
    tmpfile = write_temp_file(op, test_array_size, test_array,
                              S_IRUSR|S_IWUSR|S_IXUSR);
    
    if (!tmpfile) {
        ui_warn(op, "Unable to create temporary file for thread local "
                "storage test program (%s); assuming classic tls.",
                strerror(errno));
        goto done;
    }

    dso_tmpfile = write_temp_file(op, test_dso_array_size,
                                  test_dso_array,
                                  S_IRUSR|S_IWUSR|S_IXUSR);
    if (!dso_tmpfile) {
        ui_warn(op, "Unable to create temporary file for thread local "
                "storage test program (%s); assuming classic tls.",
                strerror(errno));
        goto done;
    }
    
    if (set_security_context(op, dso_tmpfile) == FALSE) {
        /* We are on a system with SELinux and the chcon command failed.
         * Assume that the system is recent enough to have the new TLS
         */
        ui_warn(op, "Unable to set the security context on file %s; "
                    "assuming new tls.",
                     dso_tmpfile);
        ret = TRUE;
        goto done;
    }

    /* run the test */

    cmd = nvstrcat(tmpfile, " ", dso_tmpfile, NULL);
    
    ret = run_command(op, cmd, NULL, FALSE, 0, TRUE);
    
    ret = ((ret == 0) ? TRUE : FALSE);

 done:

    if (tmpfile) {
        unlink(tmpfile);
        nvfree(tmpfile);
    }

    if (dso_tmpfile) {
        unlink(dso_tmpfile);
        nvfree(dso_tmpfile);
    }

    if (cmd) nvfree(cmd);

    return ret;

} /* test_tls_internal() */

#else /* defined(NV_TLS_TEST) */

int tls_test(Options *op, int compat_32_libs)
{
    /* Assume the TLS test passed. */
    return TRUE;
}

#endif /* defined(NV_TLS_TEST) */


/*
 * check_runtime_configuration() - In the past, nvidia-installer has
 * frequently failed to backup/move all conflicting files prior to
 * installing the NVIDIA OpenGL libraries.  Consequently, some of the
 * installations considered successful by the installer didn't work
 * correctly.
 *
 * This sanity check attemps to verify that the correct libraries are
 * picked up by the runtime linker.  It returns TRUE on success and
 * FALSE on failure.
 */

/* pull in the array and size from g_rtld_test.c */

extern const unsigned char rtld_test_array[];
extern const int rtld_test_array_size;

#if defined(NV_X86_64)

/* pull in the array and size from g_rtld_test_32.c */

extern const unsigned char rtld_test_array_32[];
extern const int rtld_test_array_32_size;

#endif /* NV_X86_64 */


/* forward prototype */

static int rtld_test_internal(Options *op, Package *p,
                              int which_tls,
                              const unsigned char *test_array,
                              const int test_array_size,
                              int compat_32_libs);

int check_runtime_configuration(Options *op, Package *p)
{
    int ret = TRUE;

    ui_status_begin(op, "Running runtime sanity check:", "Checking");

#if defined(NV_X86_64)
    ret = rtld_test_internal(op, p, op->which_tls_compat32,
                             rtld_test_array_32,
                             rtld_test_array_32_size,
                             TRUE);
#endif /* NV_X86_64 */

    if (ret == TRUE) {
        ret = rtld_test_internal(op, p, op->which_tls,
                                 rtld_test_array,
                                 rtld_test_array_size,
                                 FALSE);
    }

    ui_status_end(op, "done.");
    ui_log(op, "Runtime sanity check %s.", ret ? "passed" : "failed");

    return ret;

} /* check_runtime_configuration() */


/*
 * collapse_multiple_slashes() - remove any/all occurances of "//" from the
 * argument string.
 */

void collapse_multiple_slashes(char *s)
{
    char *p;
    unsigned int i, len;

    while ((p = strstr(s, "//")) != NULL) {
        p++; /* advance to second '/' */
        while (*p == '/') {
            len = strlen(p);
            for (i = 0; i < len; i++) p[i] = p[i+1];
        }
    }
}



/*
 * is_symbolic_link_to() - check if the file with path 'path' is
 * a symbolic link pointing to 'dest'.  Returns TRUE if this is
 * the case; if the file is not a symbolic link if it doesn't point
 * to 'dest', is_symbolic_link_to() returns FALSE.
 */

int is_symbolic_link_to(const char *path, const char *dest)
{
    struct stat stat_buf0, stat_buf1;

    if ((lstat(path, &stat_buf0) != 0)
            || !S_ISLNK(stat_buf0.st_mode))
        return FALSE;

    if ((stat(path, &stat_buf0) == 0) &&
        (stat(dest, &stat_buf1) == 0) &&
        (stat_buf0.st_dev == stat_buf1.st_dev) &&
        (stat_buf0.st_ino == stat_buf1.st_ino))
        return TRUE;

    return FALSE;

} /* is_symbolic_link_to() */



/*
 * rtld_test_internal() - this routine writes the test binaries to a file
 * and performs the test; the caller (rtld_test()) selects which array data
 * is used (native, compat_32).
 */

static int rtld_test_internal(Options *op, Package *p,
                              int which_tls,
                              const unsigned char *test_array,
                              const int test_array_size,
                              int compat_32_libs)
{
    int fd, i, found = TRUE, ret = TRUE;
    char *name = NULL, *cmd = NULL, *data = NULL;
    char *tmpfile, *s;
    char *tmpfile1 = NULL;
    struct stat stat_buf0, stat_buf1;

    if ((test_array == NULL) || (test_array_size == 0)) {
        ui_warn(op, "The runtime configuration test program is not "
                "present; assuming successful installation.");
        return TRUE;
    }

    /* write the rtld_test data to a temporary file */

    tmpfile = write_temp_file(op, test_array_size, test_array,
                              S_IRUSR|S_IWUSR|S_IXUSR);

    if (!tmpfile) {
        ui_warn(op, "Unable to create a temporary file for the runtime "
                "configuration test program (%s); assuming successful "
                "installation.", strerror(errno));
        goto done;
    }

    /* create another temporary file */

    tmpfile1 = nvstrcat(op->tmpdir, "/nv-tmp-XXXXXX", NULL);
    
    fd = mkstemp(tmpfile1);
    if (fd == -1) {
        ui_warn(op, "Unable to create a temporary file for the runtime "
                "configuration test program (%s); assuming successful "
                "installation.", strerror(errno));
        goto done;
    }
    close(fd);

    /* perform the test(s) */

    for (i = 0; i < p->num_entries; i++) {
        if (!(p->entries[i].flags & FILE_TYPE_RTLD_CHECKED))
            continue;
        else if ((which_tls & TLS_LIB_TYPE_FORCED) &&
                 (p->entries[i].flags & FILE_TYPE_TLS_LIB))
            continue;
#if defined(NV_X86_64)
        else if ((p->entries[i].flags & FILE_CLASS_NATIVE)
                 && compat_32_libs)
            continue;
        else if ((p->entries[i].flags & FILE_CLASS_COMPAT32)
                 && !compat_32_libs)
            continue;
#endif /* NV_X86_64 */
        else if ((which_tls == TLS_LIB_NEW_TLS) &&
                 (p->entries[i].flags & FILE_CLASS_CLASSIC_TLS))
            continue;
        else if ((which_tls == TLS_LIB_CLASSIC_TLS) &&
                 (p->entries[i].flags & FILE_CLASS_NEW_TLS))
            continue;

        name = nvstrdup(p->entries[i].name);
        if (!name) continue;

        s = strstr(name, ".so.1");
        if (!s || s[strlen(".so.1")] != '\0') goto next;

        cmd = nvstrcat(op->utils[LDD], " ", tmpfile, " > ", tmpfile1, NULL);

        if (run_command(op, cmd, NULL, FALSE, 0, TRUE)) {
            /* running ldd on a 32-bit SO will fail without a 32-bit loader */
            if (compat_32_libs) {
                ui_warn(op, "Unable to perform the runtime configuration "
                        "check for 32-bit library '%s' ('%s'); this is "
                        "typically caused by the lack of a 32-bit "
                        "compatibility environment.  Assuming successful "
                        "installation.", name, p->entries[i].dst);
            } else {
                ui_warn(op, "Unable to perform the runtime configuration "
                        "check for library '%s' ('%s'); assuming successful "
                        "installation.", name, p->entries[i].dst);
            }
            goto done;
        }

        cmd = nvstrcat(op->utils[GREP], " ", name, " ", tmpfile1,
                             " | ", op->utils[CUT], " -d \" \" -f 3", NULL);

        if (run_command(op, cmd, &data, FALSE, 0, TRUE) ||
                (data == NULL)) {
            ui_warn(op, "Unable to perform the runtime configuration "
                    "check for library '%s' ('%s'); assuming successful "
                    "installation.", name, p->entries[i].dst);
            goto done;
        }

        if (!strcmp(data, "not") || !strlen(data)) {
            /*
             * If the library didn't show up in ldd's output or
             * wasn't found, set 'found' to false and notify the
             * user with a more meaningful message below.
             */
            free(data); data = NULL;
            found = FALSE;
        } else {
            /*
             * Double slashes in /etc/ld.so.conf make it all the
             * way to ldd's output on some systems. Strip them
             * here to make sure they don't cause a false failure.
             */
            collapse_multiple_slashes(data);
        }

        nvfree(name); name = NULL;
        name = nvstrdup(p->entries[i].dst);
        if (!name) goto next;

        s = strstr(name, ".so.1");
        if (!s) goto next;
        *(s + strlen(".so.1")) = '\0';

        if (!found || (strcmp(data, name) != 0)) {
            /*
             * XXX Handle the case where the same library is
             * referred to, once directly and once via a symbolic
             * link. This check is far from perfect, but should
             * get the job done.
             */

            if ((stat(data, &stat_buf0) == 0) &&
                (stat(name, &stat_buf1) == 0) &&
                (stat_buf0.st_dev == stat_buf1.st_dev) &&
                (stat_buf0.st_ino == stat_buf1.st_ino))
                goto next;

            if (!found && !compat_32_libs) {
                ui_error(op, "The runtime configuration check failed for "
                         "library '%s' (expected: '%s', found: (not found)).  "
                         "The most likely reason for this is that the library "
                         "was installed to the wrong location or that your "
                         "system's dynamic loader configuration needs to be "
                         "updated.  Please check the OpenGL library installation "
                         "prefix and/or the dynamic loader configuration.",
                         p->entries[i].name, name);
                ret = FALSE;
                goto done;
#if defined(NV_X86_64)
            } else if (!found) {
                ui_warn(op, "The runtime configuration check failed for "
                        "library '%s' (expected: '%s', found: (not found)).  "
                        "The most likely reason for this is that the library "
                        "was installed to the wrong location or that your "
                        "system's dynamic loader configuration needs to be "
                        "updated.  Please check the 32-bit OpenGL compatibility "
                        "library installation prefix and/or the dynamic loader "
                        "configuration.",
                         p->entries[i].name, name);
                goto next;
#endif /* NV_X86_64 */
            } else {
                ui_error(op, "The runtime configuration check failed for the "
                         "library '%s' (expected: '%s', found: '%s').  The "
                         "most likely reason for this is that conflicting "
                         "OpenGL libraries are installed in a location not "
                         "inspected by `nvidia-installer`.  Please be sure you "
                         "have uninstalled any third-party OpenGL and/or "
                         "third-party graphics driver packages.",
                         p->entries[i].name, name, data);
                ret = FALSE;
                goto done;
            }
        }

 next:
        nvfree(name); name = NULL;
        nvfree(cmd); cmd = NULL;
        nvfree(data); data = NULL;
    }

 done:
    if (tmpfile) {
        unlink(tmpfile);
        nvfree(tmpfile);
    }
    if (tmpfile1) {
        unlink(tmpfile1);
        nvfree(tmpfile1);
    }

    nvfree(name);
    nvfree(cmd);
    nvfree(data);

    return ret;

} /* rtld_test_internal() */


/*
 * get_distribution() - determine what distribution this is; only used
 * for several bits of distro-specific behavior requested by
 * distribution maintainers.
 *
 * XXX should we provide a commandline option to override this
 * detection?
 */

Distribution get_distribution(Options *op)
{
    FILE *fp;
    char *line = NULL, *ptr;
    int eof = FALSE;

    if (access("/etc/SuSE-release", F_OK) == 0) return SUSE;
    if (access("/etc/UnitedLinux-release", F_OK) == 0) return UNITED_LINUX;
    if (access("/etc/gentoo-release", F_OK) == 0) return GENTOO;

    /*
     * Attempt to determine if the host system is 'Ubuntu Linux'
     * based by checking for a line matching DISTRIB_ID=Ubuntu in
     * the file /etc/lsb-release.
     */
    fp = fopen("/etc/lsb-release", "r");
    if (fp != NULL) {
        while (((line = fget_next_line(fp, &eof))
                    != NULL) && !eof) {
            ptr = strstr(line, "DISTRIB_ID");
            if (ptr != NULL) {
                fclose(fp);
                while (ptr != NULL && *ptr != '=') ptr++;
                if (ptr != NULL && *ptr == '=') ptr++;
                if (ptr != NULL && *ptr != '\0')
                    if (!strcasecmp(ptr, "Ubuntu")) return UBUNTU;
                break;
            }
        }
    }

    if (access("/etc/debian_version", F_OK) == 0) return DEBIAN;

    return OTHER;
    
} /* get_distribution() */



/*
 * get_xserver_information() - parse the versionString (from `X
 * -version`) and assign relevant information that we infer from the X
 * server version.
 */

static int get_xserver_information(const char *versionString,
                                   int *isModular,
                                   int *supportsOutputClassSection)
{
#define XSERVER_VERSION_FORMAT_1 "X Window System Version"
#define XSERVER_VERSION_FORMAT_2 "X.Org X Server"

    int major, minor, found;
    const char *ptr;

    /* check if this is an XFree86 X server */

    if (strstr(versionString, "XFree86 Version")) {
        *isModular = FALSE;
        return TRUE;
    }


    /*
     * This must be an X.Org X server.  Attempt to parse the major.minor version
     * out of the string
     */

    found = FALSE;

    if (((ptr = strstr(versionString, XSERVER_VERSION_FORMAT_1)) != NULL) &&
        (sscanf(ptr, XSERVER_VERSION_FORMAT_1 " %d.%d", &major, &minor) == 2)) {
        found = TRUE;
    }

    if (!found &&
        ((ptr = strstr(versionString, XSERVER_VERSION_FORMAT_2)) != NULL) &&
        (sscanf(ptr, XSERVER_VERSION_FORMAT_2 " %d.%d", &major, &minor) == 2)) {
        found = TRUE;
    }

    /* if we can't parse the version, give up */

    if (!found) return FALSE;

    /*
     * isModular: X.Org X11R6.x X servers are monolithic, all others
     * are modular
     */

    if (major == 6) {
        *isModular = FALSE;
    } else {
        *isModular = TRUE;
    }

    /*
     * support for using OutputClass sections to automatically match drivers to
     * platform devices was added in X.Org xserver 1.16.
     */
    if ((major == 6) || (major == 7) || ((major == 1) && (minor < 16))) {
        *supportsOutputClassSection = FALSE;
    } else {
        *supportsOutputClassSection = TRUE;
    }

    return TRUE;

} /* get_xserver_information() */



/*
 * query_xorg_version() - run the X binary with the '-version'
 * command line option and extract the version.
 *
 * Using the version, try to infer if it's part of a modular Xorg release. If
 * the version can't be determined, we assume it's not.
 *
 * This function assigns the following fields:
 *      op->modular_xorg
 */

#define OLD_VERSION_FORMAT "(protocol Version %d, revision %d, vendor release %d)"
#define NEW_VERSION_FORMAT "X Protocol Version %d, Revision %d, Release %d."

void query_xorg_version(Options *op)
{
    char *cmd = NULL, *data = NULL;
    int ret;

    if (!op->utils[XSERVER])
        goto done;

    cmd = nvstrcat(op->utils[XSERVER], " -version", NULL);

    if (run_command(op, cmd, &data, FALSE, 0, TRUE) ||
        (data == NULL)) {
        goto done;
    }

    /*
     * process the `X -version` output to infer if this X server is
     * modular
     */

    ret = get_xserver_information(data, &op->modular_xorg,
                                  &op->xorg_supports_output_class);

    /*
     * if get_xserver_information() failed, assume the X server is not
     * modular and does not support OutputClass sections
     */

    if (!ret) {
        op->modular_xorg = FALSE;
        op->xorg_supports_output_class = FALSE;
    }

    /* fall through */

done:
    nvfree(data);
    nvfree(cmd);
}


/*
 * check_for_running_x() - running any X server (even with a
 * non-NVIDIA driver) can cause stability problems, so check that
 * there is no X server running.  To do this, scan for any
 * /tmp/.X[n]-lock files, where [n] is the number of the X Display
 * (we'll just check for 0-7). Get the pid contained in this X lock file,
 * this is the pid of the running X server. If any X server is running, 
 * print an error message and return FALSE.  If no X server is running, 
 * return TRUE.
 */

int check_for_running_x(Options *op)
{
    char path[14], *buf;
    char procpath[17]; /* contains /proc/%d, accounts for 32-bit values of pid */
    int i, pid;

    /*
     * If we are installing for a non-running kernel *and* we are only
     * installing a kernel module, then skip this check.
     */

    if (op->kernel_module_only && op->kernel_name) {
        ui_log(op, "Only installing a kernel module for a non-running "
               "kernel; skipping the \"is an X server running?\" test.");
        return TRUE;
    }
    
    for (i = 0; i < 8; i++) {
        snprintf(path, 14, "/tmp/.X%1d-lock", i);
        if (read_text_file(path, &buf) == TRUE) {
            sscanf(buf, "%d", &pid);
            nvfree(buf);
            snprintf(procpath, 17, "/proc/%d", pid);
            if (access(procpath, F_OK) == 0) {
                ui_log(op, "The file '%s' exists and appears to contain the "
                           "process ID '%d' of a runnning X server.", path, pid);
                if (op->no_x_check) {
                    ui_log(op, "Continuing per the '--no-x-check' option.");
                } else {
                    ui_error(op, "You appear to be running an X server; please "
                                 "exit X before installing.  For further details, "
                                 "please see the section INSTALLING THE NVIDIA "
                                 "DRIVER in the README available on the Linux driver "
                                 "download page at www.nvidia.com.");
                    return FALSE;
                }
            }
        }
    }
    
    return TRUE;

} /* check_for_running_x() */


/*
 * check_for_nvidia_graphics_devices() - check if there are supported
 * NVIDIA graphics devices installed in this system. If one or more
 * supported devices are found, the function returns TRUE, else it prints
 * a warning message and returns FALSE. If legacy devices are detected
 * in the system, a warning message is printed for each one.
 */

static void ignore_libpci_output(char *fmt, ...)
{
}

int check_for_nvidia_graphics_devices(Options *op, Package *p)
{
    struct pci_access *pacc;
    struct pci_dev *dev;
    int i, found_supported_device = FALSE;
    int found_vga_device = FALSE;
    uint16 class;

    pacc = pci_alloc();
    if (!pacc) return TRUE;

    pacc->error = ignore_libpci_output;
    pacc->warning = ignore_libpci_output;
    pci_init(pacc);
    if (!pacc->methods) return TRUE;

    pci_scan_bus(pacc);

    for (dev = pacc->devices; dev != NULL; dev = dev->next) {
        if ((pci_fill_info(dev, PCI_FILL_IDENT) & PCI_FILL_IDENT) == 0)
            continue;

        class = pci_read_word(dev, PCI_CLASS_DEVICE);

        if ((class == PCI_CLASS_DISPLAY_VGA || class == PCI_CLASS_DISPLAY_3D) &&
              (dev->vendor_id == 0x10de) /* NVIDIA */ &&
              (dev->device_id >= 0x0020) /* TNT or later */) {
            /*
             * First check if this GPU is a "legacy" GPU; if it is, print a
             * warning message and point the user to the NVIDIA Linux
             * driver download page for.
             */
            int found_legacy_device = FALSE;
            for (i = 0; i < sizeof(LegacyList) / sizeof(LEGACY_INFO); i++) {
                if (dev->device_id == LegacyList[i].uiDevId) {
                    int j, nstrings;
                    const char *branch_string = "";
                    nstrings = sizeof(LegacyStrings) / sizeof(LEGACY_STRINGS);
                    for (j = 0; j < nstrings; j++) {
                        if (LegacyStrings[j].branch == LegacyList[i].branch) {
                            branch_string = LegacyStrings[j].description;
                            break;
                        }
                    }

                    ui_warn(op, "The NVIDIA %s GPU installed in this system is supported "
                            "through the NVIDIA %s legacy Linux graphics drivers.  Please "
                            "visit http://www.nvidia.com/object/unix.html for more "
                            "information.  The %s NVIDIA Linux graphics driver will "
                            "ignore this GPU.",
                            LegacyList[i].AdapterString,
                            branch_string,
                            p->version);
                    found_legacy_device = TRUE;
                }
            }

            if (!found_legacy_device) {
                found_supported_device = TRUE;

                if (class == PCI_CLASS_DISPLAY_VGA)
                    found_vga_device = TRUE;
            }
        }
    }

    dev = pacc->devices;
    pci_cleanup(pacc);
    if (!dev) return TRUE;

    if (!found_supported_device) {
        ui_warn(op, "You do not appear to have an NVIDIA GPU supported by the "
                 "%s NVIDIA Linux graphics driver installed in this system.  "
                 "For further details, please see the appendix SUPPORTED "
                 "NVIDIA GRAPHICS CHIPS in the README available on the Linux "
                 "driver download page at www.nvidia.com.", p->version);
        return FALSE;
    }

    if (!found_vga_device)
        op->no_nvidia_xconfig_question = TRUE;

    return TRUE;

} /* check_for_nvidia_graphics_devices() */


/*
 * check_selinux() - check if selinux is available.
 * Sets the variable op->selinux_enabled.
 * Returns TRUE on success, FALSE otherwise.
 */
int check_selinux(Options *op)
{
    int selinux_available = TRUE;
    
    if (op->utils[CHCON] == NULL ||
        op->utils[SELINUX_ENABLED] == NULL || 
        op->utils[GETENFORCE] == NULL) {
        selinux_available = FALSE;
    }
    
    switch (op->selinux_option) {
    case SELINUX_FORCE_YES:
        if (selinux_available == FALSE) {
            /* We have set the option --force-selinux=yes but SELinux 
             * is not available on this system */
            ui_error(op, "Invalid option '--force-selinux=yes'; "
                        "SELinux is not available on this system");
            return FALSE;
        }
        op->selinux_enabled = TRUE;
        break;
        
    case SELINUX_FORCE_NO:
        if (selinux_available == TRUE) {
            char *data = NULL;
            int ret = run_command(op, op->utils[GETENFORCE], &data, 
                                  FALSE, 0, TRUE);
            
            if ((ret != 0) || (!data)) {
                ui_warn(op, "Cannot check the current mode of SELinux; "
                             "Command getenforce() failed"); 
            } else if (!strcmp(data, "Enforcing")) {
                /* We have set the option --force-selinux=no but SELinux 
                 * is enforced on this system */
                ui_warn(op, "The option '--force-selinux' has been set to 'no', "
                            "but SELinux is enforced on this system; "
                            "The X server may not start correctly ");
            }
            nvfree(data);
        }        
        op->selinux_enabled = FALSE;
        break;
        
    case SELINUX_DEFAULT:
        op->selinux_enabled = FALSE;
        if (selinux_available == TRUE) {
            int ret = run_command(op, op->utils[SELINUX_ENABLED], NULL, 
                                  FALSE, 0, TRUE);
            if (ret == 0) {
                op->selinux_enabled = TRUE;
            }
        }
        break;
    }                 

    /* Figure out which chcon type we need if the user didn't supply one. */
    if (op->selinux_enabled && !op->selinux_chcon_type) {
        unsigned char foo = 0;
        char *tmpfile;
        static const char* chcon_types[] = {
            "textrel_shlib_t",    /* Shared library with text relocations */
            "texrel_shlib_t",     /* Obsolete synonym for the above */
            "shlib_t",            /* Generic shared library */
            NULL
        };

        /* Create a temporary file */
        tmpfile = write_temp_file(op, 1, &foo, S_IRUSR);
        if (!tmpfile) {
            ui_warn(op, "Couldn't test chcon.  Assuming shlib_t.");
            op->selinux_chcon_type = "shlib_t";
        } else {
            int i, ret;
            char *cmd;

            /* Try each chcon command */
            for (i = 0; chcon_types[i]; i++) {
                cmd = nvstrcat(op->utils[CHCON], " -t ", chcon_types[i], " ",
                               tmpfile, NULL);
                ret = run_command(op, cmd, NULL, FALSE, 0, TRUE);
                nvfree(cmd);

                if (ret == 0) break;
            }

            if (!chcon_types[i]) {
                /* None of them work! */
                ui_warn(op, "Couldn't find a working chcon argument.  "
                            "Defaulting to shlib_t.");
                op->selinux_chcon_type = "shlib_t";
            } else {
                op->selinux_chcon_type = chcon_types[i];
            }

            unlink(tmpfile);
            nvfree(tmpfile);
        }
    }

    if (op->selinux_enabled) {
        ui_log(op, "Tagging shared libraries with chcon -t %s.",
               op->selinux_chcon_type);
    }

    return TRUE;
} /* check_selinux */

/*
 * run_nvidia_xconfig() - run the `nvidia-xconfig` utility.  Without
 * any options, this will just make sure the X config file uses the
 * NVIDIA driver by default. The restore parameter controls whether
 * the --restore-original-backup option is added, which attempts to
 * restore the original backed up X config file.
 */

int run_nvidia_xconfig(Options *op, int restore)
{
    int ret, bRet = TRUE;
    char *data = NULL, *cmd, *args;

    args = restore ? " --restore-original-backup" : "";
    
    cmd = nvstrcat(find_system_util("nvidia-xconfig"), args, NULL);
    
    ret = run_command(op, cmd, &data, FALSE, 0, TRUE);
    
    if (ret != 0) {
        ui_error(op, "Failed to run `%s`:\n%s", cmd, data);
        bRet = FALSE;
    }
    
    nvfree(cmd);
    nvfree(data);

    return bRet;
    
} /* run_nvidia_xconfig() */


/*
 * run_distro_hook() - run a distribution-provided hook script
 */

int run_distro_hook(Options *op, const char *hook)
{
    int ret, status, shouldrun = op->run_distro_scripts;
    char *cmd = nvstrcat("/usr/lib/nvidia/", hook, NULL);

    if (op->kernel_module_only) {
        ui_expert(op,
                  "Not running distribution-provided %s script %s because "
                  "--kernel-module-only was specified.",
                  hook, cmd);
        ret = TRUE;
        goto done;
    }

    if (access(cmd, X_OK) < 0) {
        /* it's okay if the script doesn't exist or isn't executable */
        ui_expert(op, "No distribution %s script found.", hook);
        ret = TRUE;
        goto done;
    }

    /* in expert mode, ask before running distro hooks */
    if (op->expert) {
        shouldrun = ui_yes_no(op, shouldrun,
                              "Run distribution-provided %s script %s?",
                              hook, cmd);
    }

    if (!shouldrun) {
        ui_expert(op,
                  "Not running distribution-provided %s script %s",
                  hook, cmd);
        ret = TRUE;
        goto done;
    }

    ui_status_begin(op, "Running distribution scripts", "Executing %s", cmd);
    status = run_command(op, cmd, NULL, TRUE, 0, TRUE);
    ui_status_end(op, "done.");

    ret = (status == 0);

done:
    nvfree(cmd);
    return ret;
}



/*
 * Determine if the nouveau driver is currently in use.  We do the
 * equivalent of:
 *
 *   ls -l /sys/bus/pci/devices/ /driver | grep nouveau
 *
 * The directory structure under /sys/bus/pci/devices/ should contain
 * a directory for each PCI device, and for those devices with a
 * kernel driver there will be a "driver" symlink.
 *
 * This appears to be consistent with how libpciaccess works.
 *
 * Returns TRUE if nouveau is found; returns FALSE if not.
 */

#define SYSFS_DEVICES_PATH "/sys/bus/pci/devices"

static int nouveau_is_present(void)
{
    DIR *dir;
    struct dirent * ent;
    int found = FALSE;

    dir = opendir(SYSFS_DEVICES_PATH);

    if (!dir) {
        return FALSE;
    }

    while ((ent = readdir(dir)) != NULL) {

        char driver_path[PATH_MAX];
        char symlink_target[PATH_MAX];
        char *name;
        ssize_t ret;

        if ((strcmp(ent->d_name, ".") == 0) ||
            (strcmp(ent->d_name, "..") == 0)) {
            continue;
        }

        snprintf(driver_path, PATH_MAX,
                 SYSFS_DEVICES_PATH "/%s/driver", ent->d_name);

        driver_path[PATH_MAX - 1] = '\0';

        ret = readlink(driver_path, symlink_target, PATH_MAX);
        if (ret < 0) {
            continue;
        }

        /* readlink(3) does not nul-terminate its returned string */

        ret = NV_MIN(ret, PATH_MAX - 1);

        symlink_target[ret] = '\0';

        name = basename(symlink_target);

        if (strcmp(name, "nouveau") == 0) {
            found = TRUE;
            break;
        }
    }

    closedir(dir);

    return found;
}



static const char* modprobe_directories[] = { "/etc/modprobe.d",
                                              "/usr/lib/modprobe.d" };
#define DISABLE_NOUVEAU_FILE "/nvidia-installer-disable-nouveau.conf"

/*
 * this checksum is the result of compute_crc() for the file contents
 * written in blacklist_nouveau()
 */

#define DISABLE_NOUVEAU_FILE_CKSUM 3728279991U

/*
 * blacklist_filename() - generate the filename of a blacklist file. The
 * caller should ensure that the directory exists, or be able to handle
 * failures correctly if the directory does not exist.
 */
static char *blacklist_filename(const char *directory)
{
    return nvstrcat(directory, DISABLE_NOUVEAU_FILE, NULL);
}

static char *write_blacklist_file(const char *directory)
{
    int ret;
    struct stat stat_buf;
    FILE *file;
    char *filename;

    ret = stat(directory, &stat_buf);

    if (ret != 0 || !S_ISDIR(stat_buf.st_mode)) {
        return NULL;
    }

    filename = blacklist_filename(directory);
    file = fopen(filename, "w+");

    if (!file) {
        nvfree(filename);
        return NULL;
    }

    fprintf(file, "# generated by nvidia-installer\n");
    fprintf(file, "blacklist nouveau\n");
    fprintf(file, "options nouveau modeset=0\n");

    ret = fclose(file);

    if (ret != 0) {
        nvfree(filename);
        return NULL;
    }

    return filename;
}


/*
 * Write modprobe configuration fragments to disable loading of
 * nouveau:
 *
 *  for directory in /etc/modprobe.d /usr/lib/modprobe.d; do
 *      if [ -d $directory ]; then
 *          name=$directory/nvidia-installer-nouveau-blacklist.conf
 *          echo "# generated by nvidia-installer" > $name
 *          echo "blacklist nouveau" >> $name
 *          echo "options nouveau modeset=0" >> $name
 *      fi
 *  done
 *
 * Returns a list of written configuration files if successful; 
 * returns NULL if there was a failure.
 */

static char *blacklist_nouveau(void)
{
    int i;
    char *filelist = NULL;

    for (i = 0; i < ARRAY_LEN(modprobe_directories); i++) {
        char *filename = write_blacklist_file(modprobe_directories[i]);
        if (filename) {
            filelist = nv_prepend_to_string_list(filelist, filename, ", ");
            nvfree(filename);
        }
    }

    return filelist;
}



/*
 * Check if any nouveau blacklist file is already present with the
 * contents that we expect, and return the paths to any found files,
 * or NULL if no matching files were found
 */

static char *nouveau_blacklist_file_is_present(Options *op)
{
    int i;
    char *filelist = NULL;

    for (i = 0; i < ARRAY_LEN(modprobe_directories); i++) {
        char *filename = blacklist_filename(modprobe_directories[i]);

        if ((access(filename, R_OK) == 0) &&
            (compute_crc(op, filename) == DISABLE_NOUVEAU_FILE_CKSUM)) {
            filelist = nv_prepend_to_string_list(filelist, filename, ", ");
        }
        nvfree(filename);
    }

    return filelist;
}



/*
 * Check if the nouveau kernel driver is in use.  If it is, provide an
 * appropriate error message and offer to try to disable nouveau.
 *
 * Returns FALSE if the nouveau kernel driver is in use (cause
 * installation to abort); returns TRUE if the nouveau driver is not
 * in use.
 */

int check_for_nouveau(Options *op)
{
    int ret;
    char *blacklist_files;

#define NOUVEAU_POINTER_MESSAGE                                         \
    "Please consult the NVIDIA driver README and your Linux "           \
        "distribution's documentation for details on how to correctly " \
        "disable the Nouveau kernel driver."

    if (op->no_nouveau_check) return TRUE;

    if (!nouveau_is_present()) return TRUE;

    ui_error(op, "The Nouveau kernel driver is currently in use "
             "by your system.  This driver is incompatible with the NVIDIA "
             "driver, and must be disabled before proceeding.  "
             NOUVEAU_POINTER_MESSAGE);

    blacklist_files = nouveau_blacklist_file_is_present(op);

    if (blacklist_files) {
        ui_warn(op, "One or more modprobe configuration files to disable "
                "Nouveau are already present at: %s.  Please be "
                "sure you have rebooted your system since these files were "
                "written.  If you have rebooted, then Nouveau may be enabled "
                "for other reasons, such as being included in the system "
                "initial ramdisk or in your X configuration file.  "
                NOUVEAU_POINTER_MESSAGE, blacklist_files);
        nvfree(blacklist_files);
        return FALSE;
    }

    ret = ui_yes_no(op, op->disable_nouveau, "For some distributions, Nouveau "
                    "can be disabled by adding a file in the modprobe "
                    "configuration directory.  Would you like nvidia-installer "
                    "to attempt to create this modprobe file for you?");

    if (ret) {
        blacklist_files = blacklist_nouveau();

        if (blacklist_files) {
            ui_message(op, "One or more modprobe configuration files to "
                       "disable Nouveau, have been written.  "
                       "For some distributions, this may be sufficient to "
                       "disable Nouveau; other distributions may require "
                       "modification of the initial ramdisk.  Please reboot "
                       "your system and attempt NVIDIA driver installation "
                       "again.  Note if you later wish to reenable Nouveau, "
                       "you will need to delete these files: %s",
                       blacklist_files);
            nvfree(blacklist_files);
        } else {
            ui_warn(op, "Unable to alter the nouveau modprobe configuration.  "
                    NOUVEAU_POINTER_MESSAGE);
        }
    }

    return FALSE;
}

#define DKMS_STATUS  " status"
#define DKMS_ADD     " add"
#define DKMS_BUILD   " build"
#define DKMS_INSTALL " install"
#define DKMS_REMOVE  " remove"

/*
 * Run the DKMS tool with the provided arguments. The following operations
 * are supported:
 *
 *     DKMS_STATUS: 
 *         Check the status of the specified module.
 *     DKMS_ADD: requires version
 *         Adds the module to the DKMS database.
 *     DKMS_BUILD: requires version
 *         Builds the module against the currently running kernel.
 *     DKMS_INSTALL: requires version
 *         Installs the module for the currently running kernel.
 *     DKMS_REMOVE: reqires version
 *         Removes the module from all kernels.
 *
 * run_dkms returns TRUE if dkms is found and exits with status 0 when run;
 * FALSE if dkms can't be found, or exits with non-0 status.
 */
static int run_dkms(Options *op, const char* verb, const char *version,
                    const char *kernel, char** out)
{
    char *cmd, *cmdline, *veropt, *kernopt = NULL, *kernopt_all = "";
    const char *modopt = " -m nvidia"; /* XXX real name is in the Package */
    char *output;
    int ret;

    /* Fail if DKMS not found */
    cmd = find_system_util("dkms");
    if (!cmd) {
        if (strcmp(verb, DKMS_STATUS) != 0) {
            ui_error(op, "Failed to find dkms on the system!");
        }
        return FALSE;
    }

    /* Convert function parameters into commandline arguments. Optional
     * arguments may be NULL, in which case nvstrcat() will end early. */
    veropt = version ? nvstrcat(" -v ", version, NULL) : NULL;

    if (strcmp(verb, DKMS_REMOVE) == 0) {
        /* Always remove DKMS modules from all kernels to avoid confusion. */
        kernopt_all = " --all";
    } else {
        kernopt = kernel ? nvstrcat(" -k ", kernel, NULL) : NULL;
    }

    cmdline = nvstrcat(cmd, verb, modopt, veropt, kernopt_all, kernopt, NULL);

    nvfree(cmd);

    /* Run DKMS */
    ret = run_command(op, cmdline, &output, FALSE, 0, TRUE);
    if (ret != 0) {
        ui_error(op, "Failed to run `%s`: %s", cmdline, output);
    }

    nvfree(cmdline);
    nvfree(veropt);
    nvfree(kernopt);
    if (out) {
        *out = output;
    } else {
        nvfree(output);
    }

    return ret == 0;
}

/*
 * Check to see whether the module is installed via DKMS.
 * (The version parameter is optional: if NULL, check for any version; if
 * non-NULL, check for the specified version only.)
 *
 * Returns TRUE if DKMS is found, and dkms commandline output is non-empty.
 * Returns FALSE if DKMS not found, or dkms commandline output is empty.
 */
int dkms_module_installed(Options* op, const char *version)
{
    int ret, bRet = FALSE;
    char *output = NULL;

    ret = run_dkms(op, DKMS_STATUS, version, NULL, &output);

    if (output) bRet = strcmp("", output) != 0;
    nvfree(output);

    return ret && bRet;
}

/*
 * Install the given version of the module for the currently running kernel
 */
int dkms_install_module(Options *op, const char *version, const char *kernel)
{
    ui_status_begin(op, "Installing DKMS kernel module:", "Adding to DKMS");
    if (!run_dkms(op, DKMS_ADD, version, kernel, NULL)) goto failed;

    ui_status_update(op, .05, "Building module (This may take a moment)");
    if (!run_dkms(op, DKMS_BUILD, version, kernel, NULL)) goto failed;

    ui_status_update(op, .9, "Installing module");
    if(!run_dkms(op, DKMS_INSTALL, version, kernel, NULL)) goto failed;

    ui_status_end(op, "done.");
    return TRUE;

 failed:
    ui_status_end(op, "error.");
    ui_error(op, "Failed to install the kernel module through DKMS. No kernel "
                 "module was installed; please try installing again without "
                 "DKMS, or check the DKMS logs for more information.");
    return FALSE;
}

/*
 * Remove the given version of the module on all available kernels.
 */
int dkms_remove_module(Options *op, const char *version)
{
    return run_dkms(op, DKMS_REMOVE, version, NULL, NULL);
}
