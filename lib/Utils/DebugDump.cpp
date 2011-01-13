/*
    DebugDump.cpp

    Copyright (C) 2009  Zdenek Prikryl (zprikryl@redhat.com)
    Copyright (C) 2009  RedHat inc.

    This program is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 2 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License along
    with this program; if not, write to the Free Software Foundation, Inc.,
    51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
*/
#include <sys/utsname.h>
#include "abrtlib.h"
#include "debug_dump.h"
#include "abrt_exception.h"
#include "comm_layer_inner.h"

static bool isdigit_str(const char *str)
{
    do
    {
        if (*str < '0' || *str > '9') return false;
        str++;
    } while (*str);
    return true;
}

static std::string RemoveBackSlashes(const char *pDir)
{
    unsigned len = strlen(pDir);
    while (len != 0 && pDir[len-1] == '/')
        len--;
    return std::string(pDir, len);
}

static bool ExistFileDir(const char *pPath)
{
    struct stat buf;
    if (stat(pPath, &buf) == 0)
    {
        if (S_ISDIR(buf.st_mode) || S_ISREG(buf.st_mode))
        {
            return true;
        }
    }
    return false;
}

static void LoadTextFile(const char *pPath, std::string& pData);

CDebugDump::CDebugDump() :
    m_sDebugDumpDir(""),
    m_pGetNextFileDir(NULL),
    m_bOpened(false),
    m_bLocked(false),
    m_uid(0),
    m_gid(0)
{}

CDebugDump::~CDebugDump()
{
    /* Paranoia. In C++, destructor will abort() if it was called while unwinding
     * the stack and it throws an exception.
     */
    try
    {
        Close();
        m_sDebugDumpDir.clear();
    }
    catch (...)
    {
        error_msg_and_die("Internal error");
    }
}

void CDebugDump::Open(const char *pDir)
{
    if (m_bOpened)
    {
        throw CABRTException(EXCEP_ERROR, "CDebugDump is already opened");
    }
    m_sDebugDumpDir = RemoveBackSlashes(pDir);
    if (!ExistFileDir(m_sDebugDumpDir.c_str()))
    {
        throw CABRTException(EXCEP_DD_OPEN, "'%s' does not exist", m_sDebugDumpDir.c_str());
    }
    Lock();
    m_bOpened = true;
    /* In case caller would want to create more files, he'll need uid:gid */
    m_uid = 0;
    m_gid = 0;
    struct stat stat_buf;
    if (stat(m_sDebugDumpDir.c_str(), &stat_buf) == 0)
    {
        m_uid = stat_buf.st_uid;
        m_gid = stat_buf.st_gid;
    }
}

bool CDebugDump::Exist(const char* pPath)
{
    std::string fullPath = concat_path_file(m_sDebugDumpDir.c_str(), pPath);
    return ExistFileDir(fullPath.c_str());
}

static bool GetAndSetLock(const char* pLockFile, const char* pPID)
{
    while (symlink(pPID, pLockFile) != 0)
    {
        if (errno != EEXIST)
            perror_msg_and_die("Can't create lock file '%s'", pLockFile);

        char pid_buf[sizeof(pid_t)*3 + 4];
        ssize_t r = readlink(pLockFile, pid_buf, sizeof(pid_buf) - 1);
        if (r < 0)
        {
            if (errno == ENOENT)
            {
                /* Looks like pLockFile was deleted */
                usleep(10 * 1000); /* avoid CPU eating loop */
                continue;
            }
            perror_msg_and_die("Can't read lock file '%s'", pLockFile);
        }
        pid_buf[r] = '\0';

        if (strcmp(pid_buf, pPID) == 0)
        {
            log("Lock file '%s' is already locked by us", pLockFile);
            return false;
        }
        if (isdigit_str(pid_buf))
        {
            if (access(ssprintf("/proc/%s", pid_buf).c_str(), F_OK) == 0)
            {
                log("Lock file '%s' is locked by process %s", pLockFile, pid_buf);
                return false;
            }
            log("Lock file '%s' was locked by process %s, but it crashed?", pLockFile, pid_buf);
        }
        /* The file may be deleted by now by other process. Ignore ENOENT */
        if (unlink(pLockFile) != 0 && errno != ENOENT)
        {
            perror_msg_and_die("Can't remove stale lock file '%s'", pLockFile);
        }
    }

    VERB1 log("Locked '%s'", pLockFile);
    return true;

#if 0
/* Old code was using ordinary files instead of symlinks,
 * but it had a race window between open and write, during which file was
 * empty. It was seen to happen in practice.
 */
    int fd;
    while ((fd = open(pLockFile, O_WRONLY | O_CREAT | O_EXCL, 0640)) < 0)
    {
        if (errno != EEXIST)
            perror_msg_and_die("Can't create lock file '%s'", pLockFile);
        fd = open(pLockFile, O_RDONLY);
        if (fd < 0)
        {
            if (errno == ENOENT)
                continue; /* someone else deleted the file */
            perror_msg_and_die("Can't open lock file '%s'", pLockFile);
        }
        char pid_buf[sizeof(pid_t)*3 + 4];
        int r = read(fd, pid_buf, sizeof(pid_buf) - 1);
        if (r < 0)
            perror_msg_and_die("Can't read lock file '%s'", pLockFile);
        close(fd);
        if (r == 0)
        {
            /* Other process did not write out PID yet.
             * We HOPE it did not crash... */
            continue;
        }
        pid_buf[r] = '\0';
        if (strcmp(pid_buf, pPID) == 0)
        {
            log("Lock file '%s' is already locked by us", pLockFile);
            return -1;
        }
        if (isdigit_str(pid_buf))
        {
            if (access(ssprintf("/proc/%s", pid_buf).c_str(), F_OK) == 0)
            {
                log("Lock file '%s' is locked by process %s", pLockFile, pid_buf);
                return -1;
            }
            log("Lock file '%s' was locked by process %s, but it crashed?", pLockFile, pid_buf);
        }
        /* The file may be deleted by now by other process. Ignore errors */
        unlink(pLockFile);
    }

    int len = strlen(pPID);
    if (write(fd, pPID, len) != len)
    {
        unlink(pLockFile);
        /* close(fd); - not needed, exiting does it too */
        perror_msg_and_die("Can't write lock file '%s'", pLockFile);
    }
    close(fd);

    VERB1 log("Locked '%s'", pLockFile);
    return true;
#endif
}

void CDebugDump::Lock()
{
    if (m_bLocked)
        error_msg_and_die("Locking bug on '%s'", m_sDebugDumpDir.c_str());

    std::string lockFile = m_sDebugDumpDir + ".lock";
    char pid_buf[sizeof(long)*3 + 2];
    sprintf(pid_buf, "%lu", (long)getpid());
    while ((m_bLocked = GetAndSetLock(lockFile.c_str(), pid_buf)) != true)
    {
        sleep(1); /* was 0.5 seconds */
    }
}

void CDebugDump::UnLock()
{
    if (m_bLocked)
    {
        m_bLocked = false;
        std::string lockFile = m_sDebugDumpDir + ".lock";
        xunlink(lockFile.c_str());
        VERB1 log("UnLocked '%s'", lockFile.c_str());
    }
}

/* Create a fresh empty debug dump dir.
 *
 * Security: we should not allow users to write new files or write
 * into existing ones, but they should be able to read them.
 *
 * @param uid
 *   Crashed application's User Id
 *
 * We currently have only three callers:
 * kernel oops hook: uid=0
 *  this hook runs under 0:0
 * ccpp hook: uid=uid of crashed user's binary
 *  this hook runs under 0:0
 * python hook: uid=uid of crashed user's script
 *  this hook runs under abrt:gid
 *
 * Currently, we set dir's gid to passwd(uid)->pw_gid parameter, and we set uid to
 * abrt's user id. We do not allow write access to group.
 */
void CDebugDump::Create(const char *pDir, uid_t uid)
{
    if (m_bOpened)
    {
        throw CABRTException(EXCEP_ERROR, "DebugDump is already opened");
    }

    m_sDebugDumpDir = RemoveBackSlashes(pDir);
    if (ExistFileDir(m_sDebugDumpDir.c_str()))
    {
        throw CABRTException(EXCEP_DD_OPEN, "'%s' already exists", m_sDebugDumpDir.c_str());
    }

    Lock();
    m_bOpened = true;

    /* Was creating it with mode 0700 and user as the owner, but this allows
     * the user to replace any file in the directory, changing security-sensitive data
     * (e.g. "uid", "analyzer", "executable")
     */
    if (mkdir(m_sDebugDumpDir.c_str(), 0750) == -1)
    {
        UnLock();
        m_bOpened = false;
        throw CABRTException(EXCEP_DD_OPEN, "Can't create dir '%s'", pDir);
    }

    /* mkdir's mode (above) can be affected by umask, fix it */
    if (chmod(m_sDebugDumpDir.c_str(), 0750) == -1)
    {
        UnLock();
        m_bOpened = false;
        throw CABRTException(EXCEP_DD_OPEN, "Can't change mode of '%s'", pDir);
    }

    /* Get ABRT's user id */
    m_uid = 0;
    struct passwd *pw = getpwnam("abrt");
    if (pw)
        m_uid = pw->pw_uid;
    else
        error_msg("User 'abrt' does not exist, using uid 0");

    /* Get crashed application's group id */
    m_gid = 0;
    pw = getpwuid(uid);
    if (pw)
        m_gid = pw->pw_gid;
    else
      error_msg("User %lu does not exist, using gid 0", (long)uid);

    if (chown(m_sDebugDumpDir.c_str(), m_uid, m_gid) == -1)
    {
        perror_msg("can't change '%s' ownership to %lu:%lu", m_sDebugDumpDir.c_str(),
                   (long)m_uid, (long)m_gid);
    }

    SaveText(CD_UID, to_string(uid).c_str());

    {
        struct utsname buf;
        if (uname(&buf) != 0)
        {
            perror_msg_and_die("uname");
        }
        SaveText(FILENAME_KERNEL, buf.release);
        SaveText(FILENAME_ARCHITECTURE, buf.machine);
        std::string release;
        LoadTextFile("/etc/redhat-release", release);
        const char *release_ptr = release.c_str();
        unsigned len_1st_str = strchrnul(release_ptr, '\n') - release_ptr;
        release.erase(len_1st_str); /* usually simply removes trailing '\n' */
        SaveText(FILENAME_RELEASE, release.c_str());
    }

    time_t t = time(NULL);
    SaveText(FILENAME_TIME, to_string(t).c_str());
}

static void DeleteFileDir(const char *pDir)
{
    DIR *dir = opendir(pDir);
    if (!dir)
        return;

    struct dirent *dent;
    while ((dent = readdir(dir)) != NULL)
    {
        if (dot_or_dotdot(dent->d_name))
            continue;
        std::string fullPath = concat_path_file(pDir, dent->d_name);
        if (unlink(fullPath.c_str()) == -1)
        {
            if (errno != EISDIR)
            {
                closedir(dir);
                throw CABRTException(EXCEP_DD_DELETE, "Can't remove dir %s", fullPath.c_str());
            }
            DeleteFileDir(fullPath.c_str());
        }
    }
    closedir(dir);
    if (rmdir(pDir) == -1)
    {
        throw CABRTException(EXCEP_DD_DELETE, "Can't remove dir %s", pDir);
    }
}

void CDebugDump::Delete()
{
    if (!ExistFileDir(m_sDebugDumpDir.c_str()))
    {
        return;
    }
    DeleteFileDir(m_sDebugDumpDir.c_str());
}

void CDebugDump::Close()
{
    UnLock();
    if (m_pGetNextFileDir != NULL)
    {
        closedir(m_pGetNextFileDir);
        m_pGetNextFileDir = NULL;
    }
    m_bOpened = false;
}

static void LoadTextFile(const char *pPath, std::string& pData)
{
    FILE *fp = fopen(pPath, "r");
    if (!fp)
    {
        throw CABRTException(EXCEP_DD_LOAD, "Can't open file '%s'", pPath);
    }
    pData = "";
    int ch;
    while ((ch = fgetc(fp)) != EOF)
    {
        if (ch == '\0')
        {
            pData += ' ';
        }
        else if (isspace(ch) || (isascii(ch) && !iscntrl(ch)))
        {
            pData += ch;
        }
    }
    fclose(fp);
}

static void SaveBinaryFile(const char *pPath, const char* pData, unsigned pSize, uid_t uid, gid_t gid)
{
    /* "Why 0640?!" See ::Create() for security analysis */
    unlink(pPath);
    int fd = open(pPath, O_WRONLY | O_TRUNC | O_CREAT, 0640);
    if (fd < 0)
    {
        throw CABRTException(EXCEP_DD_SAVE, "Can't open file '%s': %s", pPath, errno ? strerror(errno) : "errno == 0");
    }
    if (fchown(fd, uid, gid) == -1)
    {
        perror_msg("can't change '%s' ownership to %lu:%lu", pPath, (long)uid, (long)gid);
    }
    unsigned r = full_write(fd, pData, pSize);
    close(fd);
    if (r != pSize)
    {
        throw CABRTException(EXCEP_DD_SAVE, "Can't save file '%s'", pPath);
    }
}

void CDebugDump::LoadText(const char* pName, std::string& pData)
{
    if (!m_bOpened)
    {
        throw CABRTException(EXCEP_DD_OPEN, "DebugDump is not opened");
    }
    std::string fullPath = concat_path_file(m_sDebugDumpDir.c_str(), pName);
    LoadTextFile(fullPath.c_str(), pData);
}

void CDebugDump::SaveText(const char* pName, const char* pData)
{
    if (!m_bOpened)
    {
        throw CABRTException(EXCEP_DD_OPEN, "DebugDump is not opened");
    }
    std::string fullPath = concat_path_file(m_sDebugDumpDir.c_str(), pName);
    SaveBinaryFile(fullPath.c_str(), pData, strlen(pData), m_uid, m_gid);
}

void CDebugDump::SaveBinary(const char* pName, const char* pData, unsigned pSize)
{
    if (!m_bOpened)
    {
        throw CABRTException(EXCEP_DD_OPEN, "DebugDump is not opened");
    }
    std::string fullPath = concat_path_file(m_sDebugDumpDir.c_str(), pName);
    SaveBinaryFile(fullPath.c_str(), pData, pSize, m_uid, m_gid);
}

void CDebugDump::InitGetNextFile()
{
    if (!m_bOpened)
    {
        throw CABRTException(EXCEP_DD_OPEN, "DebugDump is not opened");
    }
    if (m_pGetNextFileDir != NULL)
    {
        closedir(m_pGetNextFileDir);
    }
    m_pGetNextFileDir = opendir(m_sDebugDumpDir.c_str());
    if (m_pGetNextFileDir == NULL)
    {
        throw CABRTException(EXCEP_DD_OPEN, "Can't open dir '%s'", m_sDebugDumpDir.c_str());
    }
}

bool CDebugDump::GetNextFile(std::string *short_name, std::string *full_name)
{
    if (m_pGetNextFileDir == NULL)
    {
        return false;
    }

    struct dirent *dent;
    while ((dent = readdir(m_pGetNextFileDir)) != NULL)
    {
        if (is_regular_file(dent, m_sDebugDumpDir.c_str()))
        {
            if (short_name)
                *short_name = dent->d_name;
            if (full_name)
                *full_name = concat_path_file(m_sDebugDumpDir.c_str(), dent->d_name);
            return true;
        }
    }
    closedir(m_pGetNextFileDir);
    m_pGetNextFileDir = NULL;
    return false;
}

/* Utility function */
void delete_debug_dump_dir(const char *pDebugDumpDir)
{
    try
    {
        CDebugDump dd;
        dd.Open(pDebugDumpDir);
        dd.Delete();
    }
    catch (CABRTException& e)
    {
        /* Ignoring "directory already deleted" and such */
    }
}