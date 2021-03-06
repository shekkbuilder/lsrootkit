/* Warning!!: the code is bullshit (is only a beta prototype).
-
Compile: gcc -lpthread -o lsrootkit lsrootkit.c
If fails try: gcc -pthread -o lsrootkit lsrootkit.c
Execute: ./lsrootkit
Very Important: if lsrootkit process crash you can have a rootkit in the system with some bugs: memory leaks etc.
-
MIT LICENSE - Copyright (c) lsrootkit - 2013
by: David Reguera Garcia aka Dreg - dreg@fr33project.org
https://github.com/David-Reguera-Garcia-Dreg
http://www.fr33project.org

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
*/

#define _POSIX_C_SOURCE 200809L

#include <string.h>
#include <stdlib.h>
#include <time.h>
#include <stdio.h>
#include <string.h>

#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>
#include <pthread.h>
#include <limits.h>
#include <fcntl.h>
#include <sys/types.h>

#include <argp.h>

#define PROC_GID_BYTES 1000
#define LSROOT_TMP_TEMPLATE "/lsroot.XXXXXX"
#define NUM_THREADS 16 /* MUST BE POWER OF 2 */
#define EACH_DISPLAY 60000
#define MAX_VALUE(a) (((unsigned long long)1 << (sizeof(a) * CHAR_BIT)) - 1)
#define MYARRAYSIZE(a) (sizeof(a) / sizeof(*(a)))
#define SPEED_TEST "48 hours in a QUADCORE CPU 3100.000 MHz (NO SSD)"

#define _PCRED   "\x1B[31m"
#define _PCGRN   "\x1B[32m"
#define _PCYEL   "\x1B[33m"
#define _PCBLU   "\x1B[34m"
#define _PCMAG   "\x1B[35m"
#define _PCCYN   "\x1B[36m"
#define _PCWHT   "\x1B[37m"
#define _PCRESET "\x1B[0m"

#define DERROR_MSG(...) fprintf(stderr, _PCRED " [ERROR!!] " _PCRESET __VA_ARGS__ );
#define DWARNING_MSG(...) fprintf(stderr, _PCYEL " [Warning] " _PCRESET __VA_ARGS__ );
#define DOK_MSG(...) fprintf(stdout, _PCGRN " [ok] " _PCRESET __VA_ARGS__ );
#define DINFO_MSG(...) fprintf(stdout, _PCCYN " [info] " _PCRESET __VA_ARGS__ );
#define DDETECTED_MSG(...) fprintf(stdout, _PCRED " [rootkit_detected!!] " _PCRESET __VA_ARGS__ );
#define DNODETECTED_MSG(...) fprintf(stdout, _PCBLU " [NO_rootkits_detected] " _PCRESET __VA_ARGS__ );
#define DRAW_MSG(...) fprintf(stdout, __VA_ARGS__ );

struct arguments
{
    char* tmp_path;
    char* report_path;
    int disable_each_display;
    int only_processes_gid;
    int only_files_gid;
};

typedef struct THS_DAT_s
{
    char* tmp_dir;
    FILE* report_path;
    pthread_mutex_t* mutex;
    unsigned int first_gid;
    unsigned int last_gid;
    int detected;
    struct arguments* arguments;
} THD_DAT_t;


char* CreateTempDir(void);
void* BruteForceGIDProcesses(void* arg);
void* BruteForceGIDFiles(void* arg);
int main(int argc, char* argv[]);


typedef void* (*THREAD_FUNC_t)(void* arg);

int CheckRights(char* tmp_path)
{
    char test_file[PATH_MAX];
    FILE* file = NULL;
    int retf = -1;
    struct stat statbuf;
    gid_t actual_gid = 8;

    memset(test_file, 0, sizeof(test_file));
    memset(&statbuf, 0, sizeof(statbuf));

    sprintf(test_file, "%s/_test", tmp_path);

    DINFO_MSG("Checking this-process rights with file: %s\n", test_file);

    retf = -1;
    file = fopen(test_file, "wb+");
    if (file == NULL)
    {
        perror(test_file);
    }
    else
    {
        if (chown(test_file, 0, 0) == 0)
        {
            statbuf.st_gid = 8;
            if (stat(test_file, &statbuf) == 0)
            {
                if (statbuf.st_gid == 0)
                {
                    if (chown(test_file, 1, 1) == 0)
                    {
                        statbuf.st_gid = 8;
                        if (stat(test_file, &statbuf) == 0)
                        {
                            if (statbuf.st_gid == 1)
                            {
                                actual_gid = getgid();
                                if (setgid(0) == 0)
                                {
                                    if (getgid() == 0)
                                    {
                                        if (setgid(1) == 0)
                                        {
                                            if (getgid() == 1)
                                            {
                                                DOK_MSG("Rights ok!!\n");
                                                retf = 0;
                                            }
                                        }
                                    }
                                }
                                setgid(actual_gid);
                            }
                        }
                    }
                }
            }
        }

        fclose(file);
        unlink(test_file);
    }

    if (retf != 0)
    {
        DERROR_MSG("Rights fail!!\n");
    }

    return retf;
}


static inline int ExistFileInDir(char* path, char* pid_string, int* exist)
{
    DIR* dir;
    struct dirent* ent;

    *exist = 0;

    if ((dir = opendir(path)) != NULL)
    {
        while ((ent = readdir(dir)) != NULL)
        {
            if (ent->d_name[0] == '.')
            {
                continue;
            }
            if (strcmp(ent->d_name, pid_string) == 0)
            {
                *exist = 1;
                break;
            }
        }
        closedir(dir);
    }
    else
    {
        return -1;
    }

    return 0;
}



static inline int GetGIDFromPID(unsigned int* gid, char* procfs_status_file_name)
{
    char buf[PROC_GID_BYTES];
    char* aux;
    int procfs_pid;
    int retf = -1;
    ssize_t read_ret = 0;

    *gid = 0;

    procfs_pid = open(procfs_status_file_name, O_RDONLY);
    if (procfs_pid == -1)
    {
        return -1;
    }
    read_ret = read(procfs_pid, buf, sizeof(buf) - 1);
    if ((read_ret != -1) && (read_ret != 0))
    {
        buf[sizeof(buf) - 1] = '\0';
        aux = buf;
        do
        {
            aux = strchr(aux + 1, '\n');
            if (aux != NULL)
            {
                aux++;
                if (aux[0] == 'G')
                {
                    if (sscanf(aux, "Gid:\t%u\t", gid) == 1)
                    {
                        retf = 0;
                        break;
                    }
                }
            }
        } while ((aux != NULL) && (aux[0] != '\0'));
    }

    close(procfs_pid);

    return retf;
}

static inline char* CheckRootkitFilesGID(int chown_ret, int stat_ret, int exist_file_ret, int exist_in_tmp, unsigned int gid_detected, unsigned int actual_gid, unsigned int last_gid)
{
    char* rootkit_msg_detection = NULL;
    char* type = NULL;

    if (exist_file_ret == -1)
    {
        type = (char*) "tmp dir innaccesible";
    }
    else if (exist_in_tmp == 0)
    {
        type = (char*) "gid hidden from readdir tmp";
    }
    else if (chown_ret == -1)
    {
        type = (char*) "chown hooked";
    }
    else if (stat_ret == -1)
    {
        type = (char*) "stat hooked";
    }
    else if (gid_detected != actual_gid)
    {
        type = (char*) "gid_detected != actual_gid";
    }
    else if (gid_detected == last_gid)
    {
        type = (char*) "gid_detected == last_gid";
    }
    else if (gid_detected == 0)
    {
        type = (char*) "gid_detected == 0";
    }
    else
    {
        return NULL;
    }

    if (type == NULL)
    {
        return NULL;
    }

    /* I am too lazy to include a portable POSIX asprintf x) */
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wimplicit-function-declaration"
    if (asprintf(&rootkit_msg_detection, "type: %s - extra info: chown_ret: %d, stat_ret: %d, exist_file_ret: %d, exist_in_tmp: %d, gid_detected: %u, actual_gid: %u, last_gid: %u\n",
                 type, chown_ret, stat_ret, exist_file_ret, exist_in_tmp, gid_detected, actual_gid, last_gid) == -1)
    {
        return NULL;
    }
#pragma GCC diagnostic pop

    return rootkit_msg_detection;
}

void* BruteForceGIDFiles(void* arg)
{
    THD_DAT_t* th_dat = (THD_DAT_t*)arg;
    char file_name[PATH_MAX];
    char full_path[PATH_MAX];
    unsigned int gid_detected = 0;
    unsigned int actual_gid = 0;
    unsigned int last_gid = 0;
    uid_t my_uid;
    struct stat statbuf;
    unsigned int i = EACH_DISPLAY;
    char* rootkit_msg_detection = NULL;
    int chown_ret = 0;
    int stat_ret = 0;
    int exist_file_ret;
    int exist_in_tmp;
    char file_name_ext[PATH_MAX];

    my_uid = getuid();

    memset(file_name, 0, sizeof(file_name));
    memset(full_path, 0, sizeof(full_path));
    memset(file_name_ext, 0, sizeof(file_name_ext));

    sprintf(file_name, "%llu", (unsigned long long) pthread_self());

    sprintf(file_name_ext, "%s.files", file_name);

    sprintf(full_path, "%s/%s", th_dat->tmp_dir, file_name_ext);

    DOK_MSG("t[%llu] - BruteForceGIDFiles New thread! GID range: %u - %u\n\t%s\n", (unsigned long long) pthread_self(), th_dat->first_gid, th_dat->last_gid, full_path);

    fclose(fopen(full_path, "wb+"));

    actual_gid = th_dat->first_gid;
    do
    {
        chown_ret = chown(full_path, my_uid, actual_gid);
        gid_detected = 0;
        statbuf.st_gid = 0;
        stat_ret = stat(full_path, &statbuf);
        if (stat_ret != -1)
        {
            last_gid = gid_detected;
            gid_detected = statbuf.st_gid;
        }
        exist_in_tmp = 0;
        exist_file_ret = ExistFileInDir(th_dat->tmp_dir, file_name_ext, &exist_in_tmp);

        rootkit_msg_detection = CheckRootkitFilesGID(chown_ret, stat_ret, exist_file_ret, exist_in_tmp, gid_detected, actual_gid, last_gid);
        if (rootkit_msg_detection != NULL)
        {
            DDETECTED_MSG("t[%llu] - BruteForceGIDFiles rootkit detected!! %s\n\n", (unsigned long long) pthread_self(), rootkit_msg_detection);

            pthread_mutex_lock(th_dat->mutex);
            fprintf(th_dat->report_path, "\n%s\n", rootkit_msg_detection);
            fflush(th_dat->report_path);
            pthread_mutex_unlock(th_dat->mutex);
            th_dat->detected = 1;

            free(rootkit_msg_detection);
            rootkit_msg_detection = NULL;

            break;
        }

        if (th_dat->arguments->disable_each_display == 0)
        {
            if ((i++ % EACH_DISPLAY) == 0)
            {
                DINFO_MSG("t[%llu] - BruteForceGIDFiles last GID: %u (showing each %d checks.) Be patient: %s\n", (unsigned long long) pthread_self(), gid_detected, EACH_DISPLAY, SPEED_TEST);
            }
        }
    } while (actual_gid++ != th_dat->last_gid);

    unlink(full_path);

    return NULL;
}

static inline char* CheckRootkit(int exist_in_proc_ret,
                                 int exist_in_proc,
                                 int proc_ret,
                                 unsigned int gid_from_proc,
                                 unsigned int gid_detected,
                                 unsigned int actual_gid,
                                 unsigned int last_gid)
{
    char* rootkit_msg_detection = NULL;
    char* type = NULL;

    if (exist_in_proc_ret == -1)
    {
        type = (char*) "/proc dir innaccesible";
    }
    else if (exist_in_proc == 0)
    {
        type = (char*) "gid hidden from readdir proc";
    }
    else if (proc_ret == -1)
    {
        type = (char*) "gid hidden from open proc/pid";
    }
    else if (gid_from_proc != gid_detected)
    {
        type = (char*) "gid_from_proc != gid_detected";
    }
    else if (gid_detected != actual_gid)
    {
        type = (char*) "gid_detected != actual_gid";
    }
    else if (gid_detected == last_gid)
    {
        type = (char*) "gid_detected == last_gid";
    }
    else if (gid_detected == 0)
    {
        type = (char*) "gid_detected == 0";
    }
    else
    {
        return NULL;
    }

    if (type == NULL)
    {
        return NULL;
    }

    /* I am too lazy to include a portable POSIX asprintf x) */
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wimplicit-function-declaration"
    if (asprintf(&rootkit_msg_detection, "type: %s - extra info: exist_in_proc_ret: %d, exist_in_proc: %d, proc_ret: %d, gid_from_proc: %u, gid_detected: %u, actual_gid: %u, last_gid: %u",
                 type, exist_in_proc_ret, exist_in_proc, proc_ret, gid_from_proc, gid_detected, actual_gid, last_gid) == -1)
    {
        return NULL;
    }
#pragma GCC diagnostic pop

    return rootkit_msg_detection;
}


void _Parent(pid_t child_pid, int fd_child, int fd_parent, THD_DAT_t* th_dat)
{
    ssize_t write_ret = 0;
    ssize_t read_ret = 0;
    int proc_ret = 0;
    unsigned int gid_detected = 0;
    unsigned int last_gid = 0;
    unsigned int actual_gid = 0;
    int read_state = 0;
    char procfs_status_file_name[PATH_MAX];
    unsigned int gid_from_proc = 0;
    char* rootkit_msg_detection;
    int i = 0;
    char pid_string[PATH_MAX];
    int exist_in_proc;
    int exist_in_proc_ret;

    memset(procfs_status_file_name, 0, sizeof(procfs_status_file_name));
    sprintf(procfs_status_file_name, "/proc/%d/status", child_pid);

    memset(pid_string, 0, sizeof(pid_string));
    sprintf(pid_string, "%d", child_pid);

    read_state = 1;

    actual_gid = th_dat->first_gid;
    do
    {
        last_gid = gid_detected;
        read_ret = read(fd_child, &gid_detected, sizeof(gid_detected));
        gid_from_proc = 0;
        proc_ret = GetGIDFromPID(&gid_from_proc, procfs_status_file_name);
        exist_in_proc = 0;
        exist_in_proc_ret = ExistFileInDir((char*)"/proc/", pid_string, &exist_in_proc);

        write_ret = write(fd_parent, &read_state, sizeof(read_state));
        if ((read_ret == -1) || (read_ret == 0))
        {
            DWARNING_MSG("t[%llu] - BruteForceGIDProcesses broken read fd_child pipe with child!! the GID range of this thread will be stopped...\n", (unsigned long long) pthread_self());
            break;
        }
        if ((write_ret == -1) || (write_ret == 0))
        {
            DWARNING_MSG("t[%llu] - BruteForceGIDProcesses broken write fd_parent pipe with child!! the GID range of this thread will be stopped... \n", (unsigned long long) pthread_self());
            break;
        }

        rootkit_msg_detection = CheckRootkit(exist_in_proc_ret, exist_in_proc, proc_ret, gid_from_proc, gid_detected, actual_gid, last_gid);
        if (rootkit_msg_detection != NULL)
        {
            DDETECTED_MSG("t[%llu] - BruteForceGIDProcesses rootkit detected!! %s\n\n", (unsigned long long) pthread_self(), rootkit_msg_detection);

            pthread_mutex_lock(th_dat->mutex);
            fprintf(th_dat->report_path, "\n%s\n", rootkit_msg_detection);
            fflush(th_dat->report_path);
            pthread_mutex_unlock(th_dat->mutex);
            th_dat->detected = 1;

            free(rootkit_msg_detection);
            rootkit_msg_detection = NULL;

            break;
        }

        if (th_dat->arguments->disable_each_display == 0)
        {
            if ((i++ % EACH_DISPLAY) == 0)
            {
                DINFO_MSG("t[%llu] - BruteForceGIDProcesses last GID: %u (showing each %d checks.) Be patient: %s\n", (unsigned long long) pthread_self(), gid_detected, EACH_DISPLAY, SPEED_TEST);
            }
        }

    } while (actual_gid++ != th_dat->last_gid);
}

void _Child(int fd_child, int fd_parent, THD_DAT_t* th_dat)
{
    ssize_t write_ret = 0;
    ssize_t read_ret = 0;
    int set_gid_ret = 0;
    unsigned int gid_detected = 0;
    unsigned int last_gid = 0;
    unsigned int actual_gid = 0;
    int read_state = 0;
    unsigned int gid_aux = 0;

    actual_gid = th_dat->first_gid;
    do
    {
        gid_detected = 0;
        set_gid_ret = setgid(actual_gid);
        last_gid = gid_detected;
        gid_detected = getgid();

        write_ret = write(fd_child, &gid_detected, sizeof(gid_detected));
        read_ret = read(fd_parent, &read_state, sizeof(read_state));

        if ((write_ret == 0) || (write_ret == -1))
        {
            break;
        }
        if ((read_ret == 0) || (read_ret == -1))
        {
            break;
        }

        if ((actual_gid != gid_detected) ||
                (last_gid == gid_detected) ||
                (set_gid_ret != 0))
        {
            /* possible rootkit detected */
            gid_aux = 0;
            write(fd_child, &gid_aux, sizeof(gid_aux));

            break;
        }
    } while (actual_gid++ != th_dat->last_gid);
}


void Parent(pid_t child_pid, char* fifo_child, char* fifo_parent, THD_DAT_t* th_dat)
{
    int fd_child = 0;
    int fd_parent = 0;

    fd_child = open(fifo_child, O_RDONLY);
    if (fd_child != -1)
    {
        fd_parent = open(fifo_parent, O_WRONLY);
        if (fd_parent != -1)
        {
            _Parent(child_pid, fd_child, fd_parent, th_dat);

            close(fd_parent);
        }
        close(fd_child);
    }
}

void Child(char* fifo_child, char* fifo_parent, THD_DAT_t* th_dat)
{
    int fd_child = 0;
    int fd_parent = 0;

    fd_child = open(fifo_child, O_WRONLY);
    if (fd_child != -1)
    {
        fd_parent = open(fifo_parent, O_RDONLY);
        if (fd_parent != -1)
        {
            _Child(fd_child, fd_parent, th_dat);

            close(fd_parent);
        }
        close(fd_child);
    }
}

void* BruteForceGIDProcesses(void* arg)
{
    THD_DAT_t* th_dat = (THD_DAT_t*)arg;
    char fifo_name[PATH_MAX];
    char fifo_parent[PATH_MAX];
    char fifo_child[PATH_MAX];
    pid_t child_pid;

    memset(fifo_name, 0, sizeof(fifo_name));
    memset(fifo_parent, 0, sizeof(fifo_parent));
    memset(fifo_child, 0, sizeof(fifo_child));

    sprintf(fifo_name, "%llu", (unsigned long long) pthread_self());

    sprintf(fifo_parent, "%s/%s.parent_processes", th_dat->tmp_dir, fifo_name);
    sprintf(fifo_child, "%s/%s.child_processes", th_dat->tmp_dir, fifo_name);

    DOK_MSG("t[%llu] - BruteForceGIDProcesses New thread! GID range: %u - %u\n\t%s \n\t%s\n", (unsigned long long) pthread_self(), th_dat->first_gid, th_dat->last_gid, fifo_parent, fifo_child);

    if (mkfifo(fifo_parent, 0666) == 0)
    {
        if (mkfifo(fifo_child, 0666) == 0)
        {
            child_pid = fork();
            if (child_pid != -1)
            {
                if (child_pid == 0)
                {
                    Child(fifo_child, fifo_parent, th_dat);
                }
                else
                {
                    Parent(child_pid, fifo_child, fifo_parent, th_dat);
                }
            }
            unlink(fifo_child);
        }
        unlink(fifo_parent);
    }

    return NULL;
}

const char* argp_program_version = "lsrootkit beta0.1";
const char* argp_program_bug_address = "dreg@fr33project.org";

static char doc[] =
    "lsrootkit options (all analysis are ON by default):\
\v-";

static char args_doc[] = " ";

enum CMD_OPT_e
{
    OPT_EMPTY = 1,
    OPT_TMP_PATH,
    OPT_REPORT_PATH,
    OPT_DISABLE_EACH_DISPLAY,
    OPT_ONLY_PROCESSES_GID,
    OPT_ONLY_FILES_GID,
};

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
static struct argp_option options[] =
{
    {
        "tmp-path", OPT_TMP_PATH, "FILE", OPTION_ARG_OPTIONAL,
        "Set new temp path dir. Example: --tmp-path=/var/tmp"
    },
    {
        "report-path", OPT_REPORT_PATH, "FILE", OPTION_ARG_OPTIONAL,
        "Set new report path. it needs also the name. Example: --report-path=/root/analysis.txt"
    },
    { "disable-each-display", OPT_DISABLE_EACH_DISPLAY, 0, OPTION_ARG_OPTIONAL, "Disable each display messages" },
    { "only-gid-processes", OPT_ONLY_PROCESSES_GID, 0, OPTION_ARG_OPTIONAL, "Only bruteforce processes GID" },
    { "only-gid-files", OPT_ONLY_FILES_GID, 0, OPTION_ARG_OPTIONAL, "Only bruteforce files GID" },

    { 0 }
};
#pragma GCC diagnostic pop



static error_t parse_opt(int key, char* arg, struct argp_state* state)
{
    struct arguments* arguments = (struct arguments*) state->input;

    switch (key)
    {
    case OPT_TMP_PATH:
        arguments->tmp_path = arg;
        break;

    case OPT_REPORT_PATH:
        arguments->report_path = arg;
        break;

    case OPT_DISABLE_EACH_DISPLAY:
        arguments->disable_each_display = 1;
        break;

    case OPT_ONLY_PROCESSES_GID:
        arguments->only_processes_gid = 1;
        break;

    case OPT_ONLY_FILES_GID:
        arguments->only_files_gid = 1;
        break;

    case ARGP_KEY_ARG:
        break;

    default:
        return ARGP_ERR_UNKNOWN;
    }

    return 0;
}

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
static struct argp argp = { options, parse_opt, args_doc, doc };
#pragma GCC diagnostic pop

int mainw(struct arguments* arguments);

int main(int argc, char* argv[])
{
    struct arguments arguments;

    DRAW_MSG(" \n--\n"
             _PCCYN " %s - Rootkit Detector for UNIX\n" _PCRESET
             "-\n"
             "MIT LICENSE - Copyright(c) 2013\n"
             "by David Reguera Garcia aka Dreg - dreg@fr33project.org\n"
             "https://github.com/David-Reguera-Garcia-Dreg\n"
             "http://www.fr33project.org\n"
             "-\n"
             "For program help type: %s --help\n"
             "-\n"
             "Features:\n"
             "\t - Processes: Full GIDs process occupation (processes GID bruteforcing)\n"
             "\t - Files: Full GIDs file occupation (files GID bruteforcing)\n"
             "\n"
             _PCRED " Warning!!: each analysis-feature can take: %s.\n " _PCRESET
             "\n"
             _PCYEL " Very Important: if lsrootkit process crash you can have a rootkit in the system with some bugs: memory leaks etc.\n " _PCRESET
             "--\n\n"
             ,
             argp_program_version,
             argv[0],
             SPEED_TEST
            );

    memset(&arguments, 0, sizeof(arguments));

    arguments.tmp_path = NULL;
    arguments.report_path = NULL;
    arguments.disable_each_display = 0;
    arguments.only_processes_gid = 0;
    arguments.only_files_gid = 0;

    argp_parse(&argp, argc, argv, 0, 0, &arguments);

    return mainw(&arguments);
}

int RunAnalysis(THD_DAT_t* thread_data, THREAD_FUNC_t analysis_func)
{
    int detected = 0;
    pthread_t threads[NUM_THREADS];
    int t;

    memset(threads, 0, sizeof(threads));

    for (t = 0; t < NUM_THREADS; t++)
    {
        pthread_create(&(threads[t]), NULL, analysis_func, (void*)(thread_data + t));
    }

    detected = 0;
    for (t = 0; t < NUM_THREADS; t++)
    {
        pthread_join(threads[t], NULL);
        if ((thread_data + t)->detected != 0)
        {
            detected = 1;
        }
    }

    return detected;
}


int mainw(struct arguments* arguments)
{
    char* dir_name = NULL;
    THD_DAT_t th_dat[NUM_THREADS];
    int t;
    unsigned int actual_first_gid = 0;
    unsigned int distance_between_gids = (MAX_VALUE(gid_t) / NUM_THREADS);
    char report_path[PATH_MAX];
    char report_path_time[PATH_MAX];
    FILE* file_report = NULL;
    int detected = 0;
    pthread_mutex_t mutex;
    THREAD_FUNC_t anal_funcs[2];

    pthread_mutex_init(&mutex, NULL);

    memset(th_dat, 0, sizeof(th_dat));
    memset(report_path, 0, sizeof(report_path));
    memset(anal_funcs, 0, sizeof(anal_funcs));

    if (arguments->tmp_path == NULL)
    {
        dir_name = CreateTempDir();
    }
    else
    {
        dir_name = arguments->tmp_path;
    }

    DOK_MSG("tmp path: %s\n", dir_name);

    if (CheckRights(dir_name) == 0)
    {
        if (arguments->report_path == NULL)
        {
            if (getcwd(report_path, sizeof(report_path)) == NULL)
            {
                report_path[0] = '.';
            }
            if (report_path[0] == '(')
            {
                perror(report_path);
                memset(report_path, 0, sizeof(report_path));
                report_path[0] = '.';
            }

            strcat(report_path, "/lsrootkit_report");

            do
            {
                memset(report_path_time, 0, sizeof(report_path_time));
                sprintf(report_path_time, "%s_%llu", report_path, (unsigned long long) time(NULL));
                DINFO_MSG("Trying open report path: %s\n", report_path_time);
                file_report = fopen(report_path_time, "wb+");
                if (NULL == file_report)
                {
                    DERROR_MSG("oppening: %s\n", report_path_time);
                    perror("");
                    sleep(1);
                }
            } while (NULL == file_report);
        }
        else
        {
            memset(report_path_time, 0, sizeof(report_path_time));
            strcpy(report_path_time, arguments->report_path);
            file_report = fopen(report_path_time, "wb+");
        }

        if (file_report == NULL)
        {
            DERROR_MSG("openning file report: %s\n", report_path_time);
            perror("");
        }
        else
        {
            DOK_MSG("Report path Open: %s\n", report_path_time);

            DINFO_MSG("Static info:\n\tNumber of threads: %d\n\tEach display msg interval: %d\n\tGID range: 1 - %llu\n\tMax bytes reserved to found GID entry in /proc/pid/status: %u \n\n", NUM_THREADS, EACH_DISPLAY, MAX_VALUE(gid_t), PROC_GID_BYTES);

            for (t = 0; t < NUM_THREADS; t++)
            {
                th_dat[t].arguments = arguments;
                th_dat[t].mutex = &mutex;
                th_dat[t].report_path = file_report;
                th_dat[t].tmp_dir = dir_name;
                th_dat[t].first_gid = actual_first_gid + 1;
                actual_first_gid += distance_between_gids;
                th_dat[t].last_gid = actual_first_gid;
                if ((t + 1) == NUM_THREADS)
                {
                    th_dat[t].last_gid = MAX_VALUE(gid_t);
                }
                DINFO_MSG("Thread: %d, GID range: %u - %u\n", t + 1, th_dat[t].first_gid, th_dat[t].last_gid);
            }

            puts("\n");

            memset(anal_funcs, 0, sizeof(anal_funcs));
            if (arguments->only_files_gid)
            {
                anal_funcs[0] = BruteForceGIDFiles;
            }
            else if (arguments->only_processes_gid)
            {
                anal_funcs[0] = BruteForceGIDProcesses;
            }
            else
            {
                anal_funcs[0] = BruteForceGIDProcesses;
                anal_funcs[1] = BruteForceGIDFiles;
            }

            detected = 0;
            for (t = 0; (unsigned int)t < MYARRAYSIZE(anal_funcs); t++)
            {
                if (anal_funcs[t] != NULL)
                {
                    if (RunAnalysis((THD_DAT_t*)&th_dat, anal_funcs[t]) == 1)
                    {
                        detected = 1;
                        break;
                    }
                }
            }

            DINFO_MSG("Result Analysis: %s\n\n", report_path_time);
            fprintf(file_report, "\n\nResult Analysis: \n\n");
            if (detected != 0)
            {
                DDETECTED_MSG("WARNING!!! POSSIBLE ROOTKIT DETECTED!!\n\n");
                fprintf(file_report, "\n\nWARNING!!! POSSIBLE ROOTKIT DETECTED!!\n\n");
            }
            else
            {
                DNODETECTED_MSG("OK - NO ROOTKITS DETECTED\n\n");
                fprintf(file_report, "\n\nOK - NO ROOTKITS DETECTED\n\n");
            }

            fflush(file_report);

            fclose(file_report);

            pthread_mutex_destroy(&mutex);
        }
    }
    else
    {
        DERROR_MSG("the process have not rights, run it as root or set the caps for: chown, setgid\n\n");
    }

    DINFO_MSG("Deleting temp dir: %s\n\n", dir_name);
    rmdir(dir_name);

    return 0;
}

char* CreateTempDir(void)
{
    char* tmp_path = NULL;
    char template_tmp[PATH_MAX];
    char* ret = NULL;

    memset(template_tmp, 0, sizeof(template_tmp));

    if (!(tmp_path = getenv("TMPDIR")))
    {
        if (!(tmp_path = getenv("TMP")))
        {
            if (!(tmp_path = getenv("TEMP")))
            {
                if (!(tmp_path = getenv("TEMPDIR")))
                {
                    tmp_path = (char*) "/tmp";
                }
            }
        }
    }

    if (NULL == tmp_path)
    {
        DERROR_MSG( "NULL TEMP PATH\n");
        return NULL;
    }

    DINFO_MSG("TEMP DIR: %s\n", tmp_path);

    if ((strlen(tmp_path) + sizeof(LSROOT_TMP_TEMPLATE)) > sizeof(template_tmp))
    {
        DERROR_MSG("TOO BIG SIZE TEMP PATH\n");
        return NULL;
    }

    strcpy(template_tmp, tmp_path);
    strcat(template_tmp, LSROOT_TMP_TEMPLATE);

    char* dir_name = mkdtemp(template_tmp);
    if (dir_name == NULL)
    {
        DERROR_MSG("mkdtemp failed");
        return NULL;
    }

    ret = (char*)calloc(1, strlen(dir_name) + 1);
    strcpy(ret, dir_name);

    return ret;
}


/*
Dreg notes:

ps aux | grep main  | cut -d ' ' -f6 | xargs kill -9 ; ps aux | grep main ; rm -rf ./main* && cp ./projects/newlsrootkit/main.cpp ./main.c && gcc -pedantic -Wall -Wextra -x c -std=c99 -lpthread -o main main.c && ./main --only-processes-gid


lsof -ai -g -p2068

*/
