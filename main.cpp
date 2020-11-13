// Copyright 2020 Allan Riordan Boll

#include <string>
#include <vector>
#include <unordered_set>
#include <algorithm>
#include <stdexcept>
#include <memory>
#include <filesystem>
#include <sstream>
#include <thread>
#include <list>
#include <mutex>
#include <condition_variable>
#include <any>
#include <map>

#ifdef __WXMSW__

#include <winsock2.h>

#else

#include <sys/socket.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <netdb.h>

#endif

#ifdef __WXGTK__

#include "icon/icon_48x48.xpm"

#endif

#include <stdio.h>

#include <wx/wx.h>
#include <wx/listctrl.h>
#include <wx/imaglist.h>
#include <wx/artprov.h>
#include <wx/cmdline.h>
#include <wx/utils.h>
#include <wx/stdpaths.h>
#include <wx/config.h>
#include <wx/fileconf.h>
#include <wx/preferences.h>
#include <wx/dataview.h>
#include <wx/aboutdlg.h>
#include <wx/progdlg.h>
#include <wx/activityindicator.h>
#include <wx/busyinfo.h>

#include <libssh2.h>
#include <libssh2_sftp.h>

#include "./licensestrings.h"

using std::string;
using std::to_string;
using std::vector;
using std::unordered_set;
using std::copy;
using std::unique_ptr;
using std::shared_ptr;
using std::stringstream;
using std::make_unique;
using std::make_shared;
using std::filesystem::file_time_type;
using std::filesystem::create_directories;
using std::filesystem::last_write_time;
using std::filesystem::remove;
using std::exception;
using std::thread;
using std::map;

#define BUFLEN 4096
#define ID_SET_DIR 10
#define ID_PARENT_DIR 30
#define ID_SHOW_LICENSES 40
#define ID_OPEN_SELECTED 50
#define ID_SFTP_THREAD_RESPONSE_CONNECTED 60
#define ID_SFTP_THREAD_RESPONSE_GET_DIR 70
#define ID_SFTP_THREAD_RESPONSE_NEED_PASSWD 80
#define ID_SFTP_THREAD_RESPONSE_ERROR 90
#define ID_SFTP_THREAD_RESPONSE_UPLOAD 100
#define ID_SFTP_THREAD_RESPONSE_ERROR_CONNECTION 110
#define ID_SFTP_THREAD_RESPONSE_DOWNLOAD 120
#define ID_SFTP_THREAD_RESPONSE_DOWNLOAD_FAILED 130
#define ID_SFTP_THREAD_RESPONSE_DOWNLOAD_FAILED_PERMISSION 140
#define ID_SFTP_THREAD_RESPONSE_UPLOAD_FAILED 150
#define ID_SFTP_THREAD_RESPONSE_UPLOAD_FAILED_PERMISSION 160
#define ID_SFTP_THREAD_RESPONSE_UPLOAD_FAILED_SPACE 170
#define ID_SFTP_THREAD_RESPONSE_DIR_LIST_FAILED 180
#define ID_SFTP_THREAD_RESPONSE_FILE_NOT_FOUND 190


void showException() {
    wxString error;
    try {
        throw;  // Rethrow the current exception in order to pattern match it here.
    } catch (const std::exception &e) {
        error = e.what();
    } catch (...) {
        error = "unknown error";
    }

    wxLogError("%s", error);
}


string normalize_path(string path) {
    // TODO(allan): support UTF-8 paths...
    replace(path.begin(), path.end(), '\\', '/');

    stringstream s(path);
    string segment;
    vector<string> parts;
    while (getline(s, segment, '/')) {
        if (segment.empty() || segment == ".") {
            continue;
        } else if (segment == "..") {
            if (!parts.empty()) {
                parts.pop_back();
            } else {
                continue;
            }
        } else {
            parts.push_back(segment);
        }
    }

    string r = "";
    for (int i = 0; i < parts.size(); ++i) {
        if (i == 0 && parts[0].length() == 2 && parts[0][1] == ':') {
            // Windows drive letter part.
        } else {
            r += "/";
        }
        r += parts[i];
    }

    if (r.empty()) {
        return "/";
    }

    return r;
}


// Inspired by https://st.xorian.net/blog/2012/08/go-style-channel-in-c/ .
template<class item>
class Channel {
private:
    std::list<item> queue;
    std::mutex m;
    std::condition_variable cv;
public:
    void Put(const item &i) {
        std::unique_lock<std::mutex> lock(m);
        queue.push_back(i);
        cv.notify_one();
    }

    item Get() {
        std::unique_lock<std::mutex> lock(m);
        cv.wait(lock, [&]() {
            return !queue.empty();
        });
        item result = queue.front();
        queue.pop_front();
        return result;
    }
};


class DirEntry {
public:
    string name_;
    uint64_t size_ = 0;
    uint64_t modified_ = 0;
    uint64_t mode_;
    string mode_str_;
    string owner_;
    string group_;
    bool isDir_;

    string ModifiedFormatted() {
        if (this->modified_ == 0) {
            return "";
        }
        auto t = wxDateTime((time_t) this->modified_);
        t.MakeUTC();
        return t.FormatISOCombined(' ').ToStdString();
    }
};


class DownloadFailed : public exception {
public:
    string remote_path_;

    DownloadFailed(string remote_path) : remote_path_(remote_path) {}
};

class DownloadFailedPermission : public exception {
public:
    string remote_path_;

    DownloadFailedPermission(string remote_path) : remote_path_(remote_path) {}
};

class UploadFailed : public exception {
public:
    string remote_path_;

    UploadFailed(string remote_path) : remote_path_(remote_path) {}
};

class UploadFailedPermission : public exception {
public:
    string remote_path_;

    UploadFailedPermission(string remote_path) : remote_path_(remote_path) {}
};

class UploadFailedSpace : public exception {
public:
    string remote_path_;

    UploadFailedSpace(string remote_path) : remote_path_(remote_path) {}
};

class DirListFailedPermission : public exception {
public:
    string remote_path_;

    DirListFailedPermission(string remote_path) : remote_path_(remote_path) {}
};


class FileNotFound : public exception {
public:
    string remote_path_;

    FileNotFound(string remote_path) : remote_path_(remote_path) {}
};


class ConnectionError : public exception {
public:
    string msg_;

    ConnectionError(string msg) : msg_(msg) {}
};


class SftpConnection {
private:
    LIBSSH2_SESSION *session_ = NULL;
    LIBSSH2_SFTP *sftp_session_ = NULL;
    LIBSSH2_SFTP_HANDLE *sftp_opendir_handle_ = NULL;
    LIBSSH2_SFTP_HANDLE *sftp_openfile_handle_ = NULL;
    FILE *local_file_handle_ = 0;
    int sock_ = 0;

public:
    string home_dir_ = "";
    string username_;
    string host_;
    int port_;

    SftpConnection(string username, string host, int port) {
        this->username_ = username;
        this->host_ = host;
        this->port_ = port;

        int rc;

#ifdef __WXMSW__
        WSADATA wsadata;
        rc = WSAStartup(MAKEWORD(2, 0), &wsadata);
        if (rc != 0) {
            throw ConnectionError("WSAStartup failed (" + to_string(rc) + ")");
        }
#endif

        rc = libssh2_init(-1);
        if (rc != 0) {
            throw ConnectionError("libssh2_init failed. " + this->GetLastErrorMsg());
        }

        this->sock_ = socket(AF_INET, SOCK_STREAM, 0);

        struct sockaddr_in sin;
        sin.sin_family = AF_INET;
        sin.sin_port = htons(port);
        sin.sin_addr.s_addr = inet_addr(host.c_str());
        if (sin.sin_addr.s_addr == INADDR_NONE) {
            struct hostent *remote_host = gethostbyname(host.c_str());
            if (!remote_host) {
                throw ConnectionError("failed to resolve hostname (gethostbyname failed)");
            }

            sin.sin_addr.s_addr = *reinterpret_cast<u_long *>(remote_host->h_addr_list[0]);
        }

        if (connect(this->sock_, (struct sockaddr *) (&sin), sizeof(struct sockaddr_in)) != 0) {
            throw ConnectionError("socket connect failed");
        }

        this->session_ = libssh2_session_init();
        if (!this->session_) {
            throw ConnectionError("libssh2_session_init failed. " + this->GetLastErrorMsg());
        }

        libssh2_session_set_blocking(this->session_, 1);
        libssh2_session_set_timeout(this->session_, 10 * 1000);  // TODO(allan): higher timeout?

        rc = libssh2_session_handshake(this->session_, this->sock_);
        if (rc) {
            throw ConnectionError("libssh2_session_handshake failed. " + this->GetLastErrorMsg());
        }

        // TODO(allan): verify fingerprint = libssh2_hostkey_hash(session, LIBSSH2_HOSTKEY_HASH_SHA1);
    }

    vector<DirEntry> GetDir(string path) {
        int rc;

        this->sftp_opendir_handle_ = libssh2_sftp_opendir(this->sftp_session_, path.c_str());
        if (!this->sftp_opendir_handle_) {
            if (libssh2_session_last_errno(this->session_) == LIBSSH2_ERROR_SFTP_PROTOCOL) {
                unsigned long err = libssh2_sftp_last_error(this->sftp_session_);
                if (err == LIBSSH2_FX_PERMISSION_DENIED) {
                    throw DirListFailedPermission(path);
                }

                if (err == LIBSSH2_FX_NO_SUCH_PATH || err == LIBSSH2_FX_NO_SUCH_FILE || err == LIBSSH2_FX_NO_MEDIA) {
                    throw FileNotFound(path);
                }
            }

            throw ConnectionError("libssh2_sftp_opendir failed. " + this->GetLastErrorMsg());
        }

        auto files = vector<DirEntry>();
        while (1) {
            LIBSSH2_SFTP_ATTRIBUTES attrs;
            char name[BUFLEN];
            char line[BUFLEN];
            memset(name, 0, BUFLEN);
            memset(line, 0, BUFLEN);

            rc = libssh2_sftp_readdir_ex(this->sftp_opendir_handle_, name, sizeof(name), line, sizeof(line), &attrs);
            if (rc == LIBSSH2_ERROR_EAGAIN) {
                continue;
            }
            if (rc == 0) {
                break;
            }
            if (rc < 0) {
                throw ConnectionError("libssh2_sftp_readdir_ex failed. " + this->GetLastErrorMsg());
            }

            auto d = DirEntry();

            d.name_ = string(name);
            if (d.name_ == ".") {
                continue;
            }

            if (attrs.flags & LIBSSH2_SFTP_ATTR_SIZE) {
                d.size_ = attrs.filesize;
            }
            if (attrs.flags & LIBSSH2_SFTP_ATTR_ACMODTIME) {
                d.modified_ = attrs.mtime;
            }
            if (attrs.flags & LIBSSH2_SFTP_ATTR_PERMISSIONS) {
                d.mode_ = attrs.permissions;
                d.isDir_ = attrs.permissions & S_IFDIR;
            }

            // Extract user, group and mode string from the free text line.
            stringstream s(line);
            string segment;
            vector<string> parts;
            int field_num = 0;
            while (getline(s, segment, ' ')) {
                if (segment.empty()) {
                    continue;
                }

                if (field_num == 0) {
                    if (segment.length() != 10) {
                        // Free text line was in an unexpected format.
                        break;
                    }
                    d.mode_str_ = string(segment);
                }

                if (field_num == 2) {
                    d.owner_ = string(segment);
                }

                if (field_num == 3) {
                    d.group_ = string(segment);
                }

                field_num++;
            }

            files.push_back(d);
        }

        if (this->sftp_opendir_handle_) {
            libssh2_sftp_closedir(this->sftp_opendir_handle_);
            this->sftp_opendir_handle_ = NULL;
        }

        return files;
    }

    void DownloadFile(string remote_src_path, string local_dst_path) {
        this->sftp_openfile_handle_ = libssh2_sftp_open(
                this->sftp_session_,
                remote_src_path.c_str(),
                LIBSSH2_FXF_READ,
                0);
        if (!this->sftp_openfile_handle_) {
            if (libssh2_session_last_errno(this->session_) == LIBSSH2_ERROR_SFTP_PROTOCOL) {
                unsigned long err = libssh2_sftp_last_error(this->sftp_session_);
                if (err == LIBSSH2_FX_PERMISSION_DENIED || err == LIBSSH2_FX_WRITE_PROTECT) {
                    throw DownloadFailedPermission(remote_src_path);
                }
                throw DownloadFailed(remote_src_path);
            }
            throw ConnectionError(this->GetLastErrorMsg());
        }

        this->local_file_handle_ = fopen(local_dst_path.c_str(), "wb");
        // TODO(allan): error handling for fopen.

        char buf[BUFLEN];
        while (1) {
            int rc = libssh2_sftp_read(this->sftp_openfile_handle_, buf, BUFLEN);
            if (rc > 0) {
                fwrite(buf, 1, rc, this->local_file_handle_);
                // TODO(allan): error handling for fwrite.
            } else if (rc == 0) {
                break;
            } else {
                if (libssh2_session_last_errno(this->session_) == LIBSSH2_ERROR_SFTP_PROTOCOL) {
                    throw DownloadFailed(remote_src_path);
                }
                throw ConnectionError("libssh2_sftp_read failed. " + this->GetLastErrorMsg());
            }
        }

        libssh2_sftp_close(this->sftp_openfile_handle_);

        fclose(this->local_file_handle_);
        this->local_file_handle_ = 0;
    }

    void UploadFile(string local_src_path, string remote_dst_path) {
        this->sftp_openfile_handle_ = libssh2_sftp_open(
                this->sftp_session_,
                remote_dst_path.c_str(),
                LIBSSH2_FXF_WRITE | LIBSSH2_FXF_TRUNC,
                0);
        if (!this->sftp_openfile_handle_) {
            if (libssh2_session_last_errno(this->session_) == LIBSSH2_ERROR_SFTP_PROTOCOL) {
                unsigned long err = libssh2_sftp_last_error(this->sftp_session_);
                if (err == LIBSSH2_FX_PERMISSION_DENIED || err == LIBSSH2_FX_WRITE_PROTECT) {
                    throw UploadFailedPermission(remote_dst_path);
                }
                if (err == LIBSSH2_FX_NO_SPACE_ON_FILESYSTEM) {
                    throw UploadFailedSpace(remote_dst_path);
                }
                throw UploadFailed(remote_dst_path);
            }
            throw ConnectionError(this->GetLastErrorMsg());
        }

        this->local_file_handle_ = fopen(local_src_path.c_str(), "rb");
        // TODO(allan): error handling for fopen.

        char buf[BUFLEN];
        while (1) {
            int rc = fread(buf, 1, BUFLEN, this->local_file_handle_);
            if (rc > 0) {
                int nread = rc;
                char *p = buf;
                while (nread) {
                    rc = libssh2_sftp_write(this->sftp_openfile_handle_, buf, rc);
                    if (rc < 0) {
                        if (libssh2_session_last_errno(this->session_) == LIBSSH2_ERROR_SFTP_PROTOCOL) {
                            unsigned long err = libssh2_sftp_last_error(this->sftp_session_);
                            if (err == LIBSSH2_FX_NO_SPACE_ON_FILESYSTEM) {
                                throw UploadFailedSpace(remote_dst_path);
                            }
                            throw UploadFailed(remote_dst_path);
                        }

                        throw ConnectionError("libssh2_sftp_write failed. " + this->GetLastErrorMsg());
                    }
                    p += rc;
                    nread -= rc;
                }
            } else {
                // TODO(allan): error handling for fread.
                break;
            }
        }

        libssh2_sftp_close(this->sftp_openfile_handle_);

        fclose(this->local_file_handle_);
        this->local_file_handle_ = 0;
    }

    ~SftpConnection() {
        if (this->local_file_handle_) {
            fclose(this->local_file_handle_);
        }

        if (this->sftp_openfile_handle_) {
            libssh2_sftp_close(this->sftp_openfile_handle_);
        }

        if (this->sftp_opendir_handle_) {
            libssh2_sftp_closedir(this->sftp_opendir_handle_);
        }

        if (this->sftp_session_) {
            libssh2_sftp_shutdown(this->sftp_session_);
        }

        if (this->session_) {
            libssh2_session_disconnect(this->session_, "normal shutdown");
            libssh2_session_free(this->session_);
        }

        if (this->sock_) {
#ifdef __WXMSW__
            closesocket(this->sock_);
#else
            close(this->sock_);
#endif
        }

        libssh2_exit();
    }

    bool PasswordAuth(string passwd) {
        // TODO(allan): check first if password auth is even supported...
        //// char *userauthlist = libssh2_userauth_list(this->session, username.c_str(), username.size());

        if (libssh2_userauth_password(this->session_, this->username_.c_str(), passwd.c_str())) {
            return false;
        }
        this->SftpSubsystemInit();
        return true;
    }

    bool AgentAuth() {
        // TODO(allan): check first if agent auth is even supported...
        //// char *userauthlist = libssh2_userauth_list(this->session, username.c_str(), username.size());

        LIBSSH2_AGENT *agent = libssh2_agent_init(this->session_);
        if (!agent) {
            return false;
        }

        if (libssh2_agent_connect(agent)) {
            return false;
        }

        if (libssh2_agent_list_identities(agent)) {
            return false;
        }

        struct libssh2_agent_publickey *identity, *prev_identity = NULL;
        while (1) {
            int rc = libssh2_agent_get_identity(agent, &identity, prev_identity);
            if (rc != 0) {
                return false;
            }

            if (libssh2_agent_userauth(agent, username_.c_str(), identity) == 0) {
                this->SftpSubsystemInit();
                return true;
            }

            prev_identity = identity;
        }
    }

private:
    void SftpSubsystemInit() {
        this->sftp_session_ = libssh2_sftp_init(this->session_);
        if (!this->sftp_session_) {
            throw ConnectionError("libssh2_sftp_init failed. " + this->GetLastErrorMsg());
        }

        char buf[BUFLEN];
        int rc = libssh2_sftp_realpath(this->sftp_session_, ".", buf, BUFLEN);
        if (rc < 0) {
            throw ConnectionError("libssh2_sftp_realpath failed. " + this->GetLastErrorMsg());
        }
        this->home_dir_ = string(buf);
    }

    string GetLastErrorMsg() {
        char *errmsg;
        libssh2_session_last_error(this->session_, &errmsg, NULL, 0);
        string s = string(errmsg);
        return s;
    }
};


struct SftpThreadCmdConnect {
    string username;
    string host;
    int port;
};

struct SftpThreadCmdPassword {
    string password;
};

struct SftpThreadResponseConnected {
    string home_dir;
};

struct SftpThreadCmdShutdown {
};

struct SftpThreadCmdGetDir {
    string dir;
};

struct SftpThreadResponseGetDir {
    string dir;
    vector<DirEntry> dir_list;
};

struct SftpThreadResponseError {
    string error;
};

struct SftpThreadResponseFileError {
    string remote_path;
};

struct SftpThreadCmdUpload {
    string local_path;
    string remote_path;
};

struct SftpThreadResponseUpload {
    string remote_path;
};

struct SftpThreadCmdDownload {
    string local_path;
    string remote_path;
};

struct SftpThreadResponseDownload {
    string local_path;
    string remote_path;
};

template<typename T>
void respondToUIThread(wxEvtHandler *response_dest, int id, const T &payload) {
    wxThreadEvent event(wxEVT_THREAD, id);
    event.SetPayload(payload);
    wxQueueEvent(response_dest, event.Clone());
}

void respondToUIThread(wxEvtHandler *response_dest, int id) {
    wxThreadEvent event(wxEVT_THREAD, id);
    wxQueueEvent(response_dest, event.Clone());
}

void sftpThreadFunc(wxEvtHandler *response_dest, shared_ptr<Channel<std::any>> channel) {
    unique_ptr<SftpConnection> sftp_connection;

    while (1) {
        auto msg = channel->Get();

        try {
            if (msg.type() == typeid(SftpThreadCmdShutdown)) {
                return;  // Destructor of sftp_connection will be called.
            }

            if (msg.type() == typeid(SftpThreadCmdConnect)) {
                auto m = std::any_cast<SftpThreadCmdConnect>(msg);

                sftp_connection = make_unique<SftpConnection>(m.username, m.host, m.port);

                if (!sftp_connection->AgentAuth()) {
                    respondToUIThread(response_dest, ID_SFTP_THREAD_RESPONSE_NEED_PASSWD);
                    continue;
                }

                respondToUIThread(response_dest, ID_SFTP_THREAD_RESPONSE_CONNECTED,
                                  SftpThreadResponseConnected{sftp_connection->home_dir_});
                continue;
            }

            if (msg.type() == typeid(SftpThreadCmdPassword)) {
                auto m = std::any_cast<SftpThreadCmdPassword>(msg);
                if (!sftp_connection->PasswordAuth(m.password)) {
                    respondToUIThread(response_dest, ID_SFTP_THREAD_RESPONSE_ERROR,
                                      SftpThreadResponseError{"Failed to authenticate."});
                    continue;
                }

                respondToUIThread(response_dest, ID_SFTP_THREAD_RESPONSE_CONNECTED,
                                  SftpThreadResponseConnected{sftp_connection->home_dir_});
                continue;
            }

            if (msg.type() == typeid(SftpThreadCmdGetDir)) {
                auto m = std::any_cast<SftpThreadCmdGetDir>(msg);
                SftpThreadResponseGetDir resp;
                resp.dir_list = sftp_connection->GetDir(m.dir);
                resp.dir = m.dir;
                respondToUIThread(response_dest, ID_SFTP_THREAD_RESPONSE_GET_DIR, resp);
                continue;
            }

            if (msg.type() == typeid(SftpThreadCmdDownload)) {
                auto m = std::any_cast<SftpThreadCmdDownload>(msg);
                sftp_connection->DownloadFile(m.remote_path, m.local_path);
                respondToUIThread(response_dest, ID_SFTP_THREAD_RESPONSE_DOWNLOAD,
                                  SftpThreadResponseDownload{m.local_path, m.remote_path});
                // TODO(allan): catch connection failure exceptions and re-enqueue SftpThreadCmdDownload?
                continue;
            }

            if (msg.type() == typeid(SftpThreadCmdUpload)) {
                auto m = std::any_cast<SftpThreadCmdUpload>(msg);
                sftp_connection->UploadFile(m.local_path, m.remote_path);
                respondToUIThread(response_dest, ID_SFTP_THREAD_RESPONSE_UPLOAD,
                                  SftpThreadResponseUpload{m.remote_path});
                continue;
            }
        } catch (DownloadFailed e) {
            respondToUIThread(response_dest, ID_SFTP_THREAD_RESPONSE_DOWNLOAD_FAILED,
                              SftpThreadResponseFileError{e.remote_path_});
        } catch (DownloadFailedPermission e) {
            respondToUIThread(response_dest, ID_SFTP_THREAD_RESPONSE_DOWNLOAD_FAILED_PERMISSION,
                              SftpThreadResponseFileError{e.remote_path_});
        } catch (UploadFailed e) {
            respondToUIThread(response_dest, ID_SFTP_THREAD_RESPONSE_UPLOAD_FAILED,
                              SftpThreadResponseFileError{e.remote_path_});
        } catch (UploadFailedPermission e) {
            respondToUIThread(response_dest, ID_SFTP_THREAD_RESPONSE_UPLOAD_FAILED_PERMISSION,
                              SftpThreadResponseFileError{e.remote_path_});
        } catch (UploadFailedSpace e) {
            respondToUIThread(response_dest, ID_SFTP_THREAD_RESPONSE_UPLOAD_FAILED_SPACE,
                              SftpThreadResponseFileError{e.remote_path_});
        } catch (DirListFailedPermission e) {
            respondToUIThread(response_dest, ID_SFTP_THREAD_RESPONSE_DIR_LIST_FAILED,
                              SftpThreadResponseFileError{e.remote_path_});
        } catch (FileNotFound e) {
            respondToUIThread(response_dest, ID_SFTP_THREAD_RESPONSE_FILE_NOT_FOUND,
                              SftpThreadResponseFileError{e.remote_path_});
        } catch (ConnectionError e) {
            respondToUIThread(response_dest, ID_SFTP_THREAD_RESPONSE_ERROR_CONNECTION,
                              SftpThreadResponseError{e.msg_});
        } catch (exception e) {
            respondToUIThread(response_dest, ID_SFTP_THREAD_RESPONSE_ERROR,
                              SftpThreadResponseError{e.what()});
        }
    }
}

typedef std::function<void(int)> OnItemActivatedCb;
typedef std::function<void(int)> OnColumnHeaderClickCb;

// A base class, because wxDataViewListCtrl looks best on MacOS, and wxListCtrl looks best on GTK and Windows.
class DirListCtrl {
protected:
    OnItemActivatedCb on_item_activated_cb_;
    OnColumnHeaderClickCb on_column_header_click_cb_;
    wxImageList *icons_image_list_;

    int IconIdx(DirEntry entry) {
        int r = 0;
        if (entry.isDir_) {
            r = 1;
        }
        return r;
    }

public:
    DirListCtrl() {
        auto ap = wxArtProvider();
        auto size = wxSize(16, 16);
        this->icons_image_list_ = new wxImageList(size.GetWidth(), size.GetHeight(), false, 1);
        this->icons_image_list_->Add(ap.GetBitmap(wxART_NORMAL_FILE, wxART_LIST, size));
        this->icons_image_list_->Add(ap.GetBitmap(wxART_FOLDER, wxART_LIST, size));
    }

    virtual void Refresh(vector<DirEntry> entries) = 0;

    virtual wxControl *GetCtrl() = 0;

    virtual void SetFocus() = 0;

    virtual void ActivateCurrent() = 0;

    virtual vector<int> GetSelected() = 0;

    virtual void SetSelected(vector<int>) = 0;

    virtual int GetHighlighted() = 0;

    virtual void SetHighlighted(int) = 0;

    void BindOnItemActivated(OnItemActivatedCb cb) {
        this->on_item_activated_cb_ = cb;
    }

    void BindOnColumnHeaderClickCb(OnColumnHeaderClickCb cb) {
        this->on_column_header_click_cb_ = cb;
    }
};


class DvlcDirList : public DirListCtrl {
    wxDataViewListCtrl *dvlc_;

public:
    explicit DvlcDirList(wxWindow *parent) : DirListCtrl() {
        this->dvlc_ = new wxDataViewListCtrl(parent, wxID_ANY, wxDefaultPosition, wxDefaultSize,
                                             wxDV_MULTIPLE | wxDV_ROW_LINES);

        // TODO(allan): wxDATAVIEW_CELL_EDITABLE?
        this->dvlc_->AppendIconTextColumn("Name", wxDATAVIEW_CELL_INERT, 300);
        this->dvlc_->AppendTextColumn("Size", wxDATAVIEW_CELL_INERT, 100);
        this->dvlc_->AppendTextColumn("Modified", wxDATAVIEW_CELL_INERT, 150);
        this->dvlc_->AppendTextColumn("Mode", wxDATAVIEW_CELL_INERT, 100);
        this->dvlc_->AppendTextColumn("Owner", wxDATAVIEW_CELL_INERT, 100);
        this->dvlc_->AppendTextColumn("Group", wxDATAVIEW_CELL_INERT, 100);

        this->dvlc_->Bind(wxEVT_DATAVIEW_ITEM_ACTIVATED, [&](wxDataViewEvent &evt) {
            if (!evt.GetItem()) {
                return;
            }
            int i = this->dvlc_->GetItemData(evt.GetItem());
            this->on_item_activated_cb_(i);
        });

        this->dvlc_->Bind(wxEVT_DATAVIEW_COLUMN_HEADER_CLICK, [&](wxDataViewEvent &evt) {
            this->on_column_header_click_cb_(evt.GetColumn());
        });
    }

    void Refresh(vector<DirEntry> entries) {
        this->dvlc_->DeleteAllItems();

        for (int i = 0; i < entries.size(); i++) {
            wxIcon icon = this->icons_image_list_->GetIcon(this->IconIdx(entries[i]));

            wxVector<wxVariant> data;
            data.push_back(wxVariant(wxDataViewIconText(wxString::FromUTF8(entries[i].name_), icon)));
            data.push_back(wxVariant(to_string(entries[i].size_)));
            data.push_back(wxVariant(entries[i].ModifiedFormatted()));
            data.push_back(wxVariant(entries[i].mode_str_));
            data.push_back(wxVariant(entries[i].owner_));
            data.push_back(wxVariant(entries[i].group_));
            this->dvlc_->AppendItem(data, i);
        }
    }

    wxControl *GetCtrl() {
        return this->dvlc_;
    }

    void SetFocus() {
        this->dvlc_->SetFocus();
    }

    void ActivateCurrent() {
        if (this->dvlc_->GetCurrentItem()) {
            int i = this->dvlc_->GetItemData(this->dvlc_->GetCurrentItem());
            this->on_item_activated_cb_(i);
        }
    }

    vector<int> GetSelected() {
        vector<int> r;
        for (int i = 0; i < this->dvlc_->GetItemCount(); ++i) {
            if (this->dvlc_->IsRowSelected(i)) {
                r.push_back(i);
            }
        }
        return r;
    }

    void SetSelected(vector<int> selected) {
        wxDataViewItemArray a;
        for (int i = 0; i < selected.size(); ++i) {
            a.push_back(this->dvlc_->RowToItem(selected[i]));
        }
        this->dvlc_->SetSelections(a);
    }

    int GetHighlighted() {
        int i = this->dvlc_->ItemToRow(this->dvlc_->GetCurrentItem());
        if (i < 0) {
            return 0;
        }
        return i;
    }

    void SetHighlighted(int row) {
        auto item = this->dvlc_->RowToItem(row);
        if (item.IsOk()) {
            this->dvlc_->SetCurrentItem(item);
            this->dvlc_->EnsureVisible(item);
        }
    }
};


class LcDirList : public DirListCtrl {
    wxListCtrl *list_ctrl_;

public:
    explicit LcDirList(wxWindow *parent) : DirListCtrl() {
        this->list_ctrl_ = new wxListCtrl(parent, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxLC_REPORT);

        this->list_ctrl_->AssignImageList(this->icons_image_list_, wxIMAGE_LIST_SMALL);

        this->list_ctrl_->InsertColumn(0, "Name", wxLIST_FORMAT_LEFT, 300);
        this->list_ctrl_->InsertColumn(1, "Size", wxLIST_FORMAT_LEFT, 100);
        this->list_ctrl_->InsertColumn(2, "Modified", wxLIST_FORMAT_LEFT, 150);
        this->list_ctrl_->InsertColumn(3, "Mode", wxLIST_FORMAT_LEFT, 100);
        this->list_ctrl_->InsertColumn(4, "Owner", wxLIST_FORMAT_LEFT, 100);
        this->list_ctrl_->InsertColumn(5, "Owner", wxLIST_FORMAT_LEFT, 100);

        this->list_ctrl_->Bind(wxEVT_LIST_ITEM_ACTIVATED, [&](wxListEvent &evt) {
            int i = this->list_ctrl_->GetItemData(evt.GetItem());
            this->on_item_activated_cb_(i);
        });

        this->list_ctrl_->Bind(wxEVT_LIST_COL_CLICK, [&](wxListEvent &evt) {
            this->on_column_header_click_cb_(evt.GetColumn());
        });
    }

    wxControl *GetCtrl() {
        return this->list_ctrl_;
    }

    void Refresh(vector<DirEntry> entries) {
        this->list_ctrl_->DeleteAllItems();

        for (int i = 0; i < entries.size(); i++) {
            this->list_ctrl_->InsertItem(i, entries[i].name_, this->IconIdx(entries[i]));
            this->list_ctrl_->SetItemData(i, i);
            this->list_ctrl_->SetItem(i, 0, wxString::FromUTF8(entries[i].name_));
            this->list_ctrl_->SetItem(i, 1, to_string(entries[i].size_));
            this->list_ctrl_->SetItem(i, 2, entries[i].ModifiedFormatted());
            this->list_ctrl_->SetItem(i, 3, entries[i].mode_str_);
            this->list_ctrl_->SetItem(i, 4, entries[i].owner_);
            this->list_ctrl_->SetItem(i, 5, entries[i].group_);
        }
    }

    void SetFocus() {
        this->list_ctrl_->SetFocus();
    }

    void ActivateCurrent() {
        if (this->list_ctrl_->GetSelectedItemCount() > 0) {
            int i = this->list_ctrl_->GetItemData(
                    this->list_ctrl_->GetNextItem(-1, wxLIST_NEXT_ALL, wxLIST_STATE_FOCUSED));
            this->on_item_activated_cb_(i);
        }
    }

    void SetSelected(vector<int> selected) {
        for (int i = 0; i < selected.size(); ++i) {
            this->list_ctrl_->SetItemState(selected[i], wxLIST_STATE_SELECTED, wxLIST_STATE_SELECTED);
        }
    }

    vector<int> GetSelected() {
        vector<int> r;
        int64_t cur = -1;
        while (1) {
            cur = this->list_ctrl_->GetNextItem(cur, wxLIST_NEXT_ALL, wxLIST_STATE_SELECTED);
            if (cur == -1) {
                break;
            }

            r.push_back(cur);
        }
        return r;
    }

    int GetHighlighted() {
        if (this->list_ctrl_->GetSelectedItemCount() > 0) {
            auto i = this->list_ctrl_->GetNextItem(-1, wxLIST_NEXT_ALL, wxLIST_STATE_FOCUSED);
            return this->list_ctrl_->GetItemData(i);
        }
        return 0;
    }

    void SetHighlighted(int row) {
        this->list_ctrl_->SetItemState(row, wxLIST_STATE_FOCUSED, wxLIST_STATE_FOCUSED);
        if (row != 0) {
            this->list_ctrl_->EnsureVisible(row);
        }
    }
};


class PreferencesPageGeneralPanel : public wxPanel {
    wxConfigBase *config_;
    wxTextCtrl *text_editor_;

public:
    PreferencesPageGeneralPanel(wxWindow *parent, wxConfigBase *config) : wxPanel(parent) {
        this->config_ = config;

        auto *sizer = new wxBoxSizer(wxVERTICAL);

        auto *item_sizer_editor = new wxBoxSizer(wxHORIZONTAL);
        auto *label_editor = new wxStaticText(this, wxID_ANY, "Editor path:");
        item_sizer_editor->Add(label_editor, 0, wxALL | wxALIGN_CENTER_VERTICAL, 5);
        item_sizer_editor->Add(5, 5, 1, wxALL, 0);
        this->text_editor_ = new wxTextCtrl(this, 100, wxEmptyString, wxDefaultPosition, wxSize(300, -1));
        this->Bind(wxEVT_TEXT, [&](wxCommandEvent &) {
            if (wxPreferencesEditor::ShouldApplyChangesImmediately()) {
                this->TransferDataFromWindow();
            }
        });
        item_sizer_editor->Add(this->text_editor_, 0, wxALL | wxALIGN_CENTER_VERTICAL, 5);

        sizer->Add(item_sizer_editor, 0, wxGROW | wxALL, 5);

        this->SetSizerAndFit(sizer);
    }

    virtual bool TransferDataToWindow() {
        this->text_editor_->SetValue(this->config_->Read("/editor", ""));
        return true;
    }

    virtual bool TransferDataFromWindow() {
        this->config_->Write("/editor", this->text_editor_->GetValue());
        this->config_->Flush();
        return true;
    }
};

class PreferencesPageGeneral : public wxStockPreferencesPage {
    wxConfigBase *config;

public:
    explicit PreferencesPageGeneral(wxConfigBase *config) : wxStockPreferencesPage(Kind_General) {
        this->config = config;
    }

    virtual wxWindow *CreateWindow(wxWindow *parent) {
        return new PreferencesPageGeneralPanel(parent, this->config);
    }
};


struct OpenedFile {
    string local_path;
    string remote_path;
    file_time_type modified;
    bool upload_requested = false;
};


// Make for example an error string a little easier on the eyes.
string prettifySentence(string s) {
    if (s.size() > 0) {
        s[0] = toupper(s[0]);
    }
    if (s.size() > 0 && s[s.size() - 1] != '.') {
        s += ".";
    }
    return s;
}

class SftpguiFrame : public wxFrame {
    string username_;
    string host_;
    int port_;
    wxConfigBase *config_;
    DirListCtrl *dir_list_ctrl_;
    wxTextCtrl *path_text_ctrl_;
    wxTimer file_watcher_timer_;
    string current_dir_;
    vector<DirEntry> current_dir_list_;
    int sort_column_ = 0;
    bool sort_desc_ = false;
    map<string, OpenedFile> opened_files_local_;
    string stored_highlighted_ = "";
    unordered_set<string> stored_selected_;
    unique_ptr<thread> sftp_thread_;
    shared_ptr<Channel<std::any>> sftp_thread_channel_ = make_shared<Channel<std::any>>();
    wxTimer reconnect_timer_;
    int reconnect_timer_countdown_;
    string reconnect_timer_error_ = "";
    string latest_interesting_status_ = "";

public:
    SftpguiFrame(string username, string host, int port, wxConfigBase *config) : wxFrame(
            NULL,
            wxID_ANY,
            wxT("Sftpgui"),
            wxPoint(-1, -1),
            wxSize(800, 600)
    ) {
        this->username_ = username;
        this->host_ = host;
        this->port_ = port;
        this->config_ = config;

#ifdef __WXMSW__
        this->SetIcon(wxIcon("aaaa"));
#elif __WXGTK__
        this->SetIcon(wxIcon(icon_48x48));
#endif

        this->SetTitle("Sftpgui - " + username + "@" + host + ":" + to_string(port));
        this->CreateStatusBar();

        // Create menus.
        auto *menuBar = new wxMenuBar();
        this->SetMenuBar(menuBar);

        auto *file_menu = new wxMenu();
        menuBar->Append(file_menu, "&File");

        // Adding refresh to the menu twice with two different hotkeys, instead of using SetAcceleratorTable.
        // It's wonky, but MacOS has trouble with non-menu accelerators when the wxDataViewListCtrl has focus.
        file_menu->Append(wxID_REFRESH, "Refresh\tF5");
        file_menu->Append(wxID_REFRESH, "Refresh\tCtrl+R");
        this->Bind(wxEVT_MENU, [&](wxCommandEvent &event) {
            this->latest_interesting_status_ = "";
            this->RefreshDir(this->current_dir_, true);
        }, wxID_REFRESH);

        file_menu->Append(ID_SET_DIR, "Change directory\tCtrl+L");
        this->Bind(wxEVT_MENU, [&](wxCommandEvent &) {
            this->path_text_ctrl_->SetFocus();
            this->path_text_ctrl_->SelectAll();
        }, ID_SET_DIR);

#ifdef __WXOSX__
        file_menu->Append(ID_PARENT_DIR, "Parent directory\tCtrl+Up", wxEmptyString, wxITEM_NORMAL);
#else
        file_menu->Append(ID_PARENT_DIR, "Parent directory\tAlt+Up", wxEmptyString, wxITEM_NORMAL);
#endif
        this->Bind(wxEVT_MENU, [&](wxCommandEvent &event) {
            this->RefreshDir(normalize_path(this->current_dir_ + "/.."), false);
        }, ID_PARENT_DIR);

#ifdef __WXOSX__
        file_menu->Append(ID_OPEN_SELECTED, "Open selected item\tCtrl+Down", wxEmptyString, wxITEM_NORMAL);
#endif
        this->Bind(wxEVT_MENU, [&](wxCommandEvent &event) {
            this->dir_list_ctrl_->ActivateCurrent();
        }, ID_OPEN_SELECTED);

        file_menu->AppendSeparator();

        file_menu->Append(wxID_PREFERENCES);
        this->Bind(wxEVT_MENU, [&](wxCommandEvent &event) {
            auto prefs_editor = new wxPreferencesEditor();
            prefs_editor->AddPage(new PreferencesPageGeneral(this->config_));
            prefs_editor->Show(this);
        }, wxID_PREFERENCES);

        file_menu->AppendSeparator();

        file_menu->Append(wxID_EXIT, "E&xit", "Quit this program");
        this->Bind(wxEVT_MENU, [&](wxCommandEvent &event) {
            this->Close(true);
        }, wxID_EXIT);

        auto *help_menu = new wxMenu;
        menuBar->Append(help_menu, "&Help");

        help_menu->Append(ID_SHOW_LICENSES, "Licenses");
        this->Bind(wxEVT_MENU, [&](wxCommandEvent &event) {
            wxDialog *licenses_frame = new wxDialog(this,
                                                    wxID_ANY,
                                                    "Licenses",
                                                    wxDefaultPosition,
                                                    wxSize(600, 600),
                                                    wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER);
            wxBoxSizer *sizer = new wxBoxSizer(wxVERTICAL);
            wxTextCtrl *licenses_text_ctrl = new wxTextCtrl(
                    licenses_frame,
                    wxID_ANY,
                    wxString::FromAscii(licenses),
                    wxDefaultPosition,
                    wxDefaultSize,
                    wxTE_MULTILINE | wxTE_READONLY | wxBORDER_NONE);
            sizer->Add(licenses_text_ctrl, 1, wxEXPAND | wxALL);
            licenses_frame->SetSizer(sizer);
            licenses_frame->Show();
        }, ID_SHOW_LICENSES);

        help_menu->Append(wxID_ABOUT);
        this->Bind(wxEVT_MENU, [&](wxCommandEvent &event) {
            wxAboutDialogInfo info;
            info.SetName("Sftpgui");
            info.SetVersion("0.1");
            info.SetDescription("A no-nonsense SFTP file browser");
            info.SetCopyright("(C) 2020 Allan Riordan Boll");
            wxAboutBox(info, this);
        }, wxID_ABOUT);

        // Most keyboard accelerators for menu items are automatically bound via the string in its title. However, some
        // seem to only work via SetAcceleratorTable, so setting them again here.
        // MacOS seems to ignores this table when the focus is on wxDataViewListCtrl, so we rely on the accelerators in
        // the menu item titles on MacOS.
        wxAcceleratorEntry entries[]{
                wxAcceleratorEntry(wxACCEL_NORMAL, WXK_F5, wxID_REFRESH),
                wxAcceleratorEntry(wxACCEL_CTRL, 'R', wxID_REFRESH),
                wxAcceleratorEntry(wxACCEL_CTRL, 'L', ID_SET_DIR),
                wxAcceleratorEntry(wxACCEL_ALT, WXK_UP, ID_PARENT_DIR),
        };
        wxAcceleratorTable accel(sizeof(entries), entries);
        this->SetAcceleratorTable(accel);

        // Set up a timer that will watch for changes in local files.
        this->file_watcher_timer_.Bind(wxEVT_TIMER, &SftpguiFrame::OnFileWatcherTimer, this);
        this->file_watcher_timer_.Start(1000);

        // Main layout.
        auto *panel = new wxPanel(this);
        auto *sizer = new wxBoxSizer(wxVERTICAL);
        auto *sizer_inner_top = new wxBoxSizer(wxHORIZONTAL);
        sizer->Add(sizer_inner_top, 0, wxEXPAND | wxALL, 1);

        // Create remote path text field.
        this->path_text_ctrl_ = new wxTextCtrl(panel, wxID_ANY, this->current_dir_, wxDefaultPosition,
                                               wxDefaultSize, wxTE_PROCESS_ENTER);
#ifdef __WXOSX__
        sizer_inner_top->Add(this->path_text_ctrl_, 1, wxEXPAND | wxALL, 0);
#else
        sizer_inner_top->Add(this->path_text_ctrl_, 1, wxEXPAND | wxALL, 4);
#endif
        this->path_text_ctrl_->Bind(wxEVT_TEXT_ENTER, [&](wxCommandEvent &event) {
            this->RefreshDir(this->path_text_ctrl_->GetValue().ToStdString(wxMBConvUTF8()), false);
        });
        this->path_text_ctrl_->Bind(wxEVT_CHAR_HOOK, [&](wxKeyEvent &evt) {
            if (evt.GetModifiers() == 0 && evt.GetKeyCode() == WXK_ESCAPE && this->path_text_ctrl_->HasFocus()) {
                this->path_text_ctrl_->SetValue(this->current_dir_);
                this->path_text_ctrl_->SelectNone();
                this->dir_list_ctrl_->SetFocus();
                return;
            }

            evt.Skip();
        });

#ifdef __WXOSX__
        // On MacOS wxDataViewListCtrl looks best.
        this->dir_list_ctrl_ = new DvlcDirList(panel);
#else
        // On GTK and Windows wxListCtrl looks best.
        this->dir_list_ctrl_ = new LcDirList(panel);
#endif
        sizer->Add(this->dir_list_ctrl_->GetCtrl(), 1, wxEXPAND | wxALL, 0);
        this->dir_list_ctrl_->SetFocus();

        this->dir_list_ctrl_->BindOnItemActivated([&](int n) {
            string status = "";
            try {
                auto entry = this->current_dir_list_[n];
                auto path = normalize_path(this->current_dir_ + "/" + entry.name_);
                if (entry.isDir_) {
                    this->current_dir_ = path;
                    this->path_text_ctrl_->SetValue(path);
                    this->current_dir_list_.clear();
                    this->dir_list_ctrl_->Refresh(vector<DirEntry>{});
                    this->RefreshDir(path, false);
                } else {
                    this->DownloadFile(path);
                }
            } catch (...) {
                showException();
            }
        });

        this->dir_list_ctrl_->BindOnColumnHeaderClickCb([&](int col) {
            if (this->sort_column_ == col) {
                this->sort_desc_ = !this->sort_desc_;
            } else {
                this->sort_desc_ = false;
                this->sort_column_ = col;
            }

            this->RememberSelected();
            this->SortAndPopulateDir();
            this->RecallSelected();
            this->dir_list_ctrl_->SetFocus();
        });

        panel->SetSizerAndFit(sizer);

        // Restore window size and pos. This needs to happen after all controls are added.
        int x = this->config_->Read("/window_x", -1);
        int y = this->config_->Read("/window_y", -1);
        int w = this->config_->Read("/window_w", 800);
        int h = this->config_->Read("/window_h", 600);
        this->Move(x, y);
        this->SetClientSize(w, h);

        this->Bind(wxEVT_CLOSE_WINDOW, [&](wxCloseEvent &event) {
            this->SetStatusText("Disconnecting...");

            if (this->sftp_thread_channel_) {
                this->sftp_thread_channel_->Put(SftpThreadCmdShutdown{});
                this->sftp_thread_->join();
            }

            for (auto o : this->opened_files_local_) {
                remove(o.second.local_path);
            }

            // Save frame position.
            int x, y, w, h;
            this->GetClientSize(&w, &h);
            this->GetPosition(&x, &y);
            this->config_->Write("/window_x", x);
            this->config_->Write("/window_y", y);
            this->config_->Write("/window_w", w);
            this->config_->Write("/window_h", h);
            this->config_->Flush();

            event.Skip();  // Default event handler calls Destroy().
        });

        // Timer used to count down till reconnecting when reconnect was requested.
        this->reconnect_timer_.Bind(wxEVT_TIMER, [&](wxTimerEvent &event) {
            if (this->reconnect_timer_countdown_ > 0) {
                auto s = to_string(this->reconnect_timer_countdown_);
                this->SetStatusText(
                        wxString::FromUTF8(this->reconnect_timer_error_ + " Reconnecting in " + s + " seconds..."));
                this->reconnect_timer_countdown_ -= 1;
                return;
            }
            this->reconnect_timer_.Stop();
            this->sftp_thread_channel_->Put(SftpThreadCmdConnect{this->username_, this->host_, this->port_});
            this->SetStatusText(wxString::FromUTF8(this->reconnect_timer_error_ + " Reconnecting..."));
        });

        // Start the sftp thread. We will be communicating with it only through message passing.
        this->SetupSftpThreadCallbacks();
        this->sftp_thread_ = make_unique<thread>(sftpThreadFunc, this, this->sftp_thread_channel_);
        this->sftp_thread_channel_->Put(SftpThreadCmdConnect{this->username_, this->host_, this->port_});
        if (!wxIsBusy()) {
            wxBeginBusyCursor();
        }
        this->SetStatusText("Connecting...");
    }

private:
    void SetupSftpThreadCallbacks() {
        // Sftp thread will trigger this callback after successfully connecting.
        this->Bind(wxEVT_THREAD, [&](wxThreadEvent &event) {
            auto r = event.GetPayload<SftpThreadResponseConnected>();
            if (this->current_dir_ == "") {
                this->current_dir_ = r.home_dir;
            }

            // In case this was a reconnect after a dropped connection, reset all upload_requested-flags.
            for (auto o : this->opened_files_local_) {
                this->opened_files_local_[o.first].upload_requested = false;
            }

            this->SetStatusText("Connected. Getting directory list...");
            this->RefreshDir(this->current_dir_, false);
        }, ID_SFTP_THREAD_RESPONSE_CONNECTED);

        // Sftp thread will trigger this callback if it requires a password for the connection.
        this->Bind(wxEVT_THREAD, [&](wxThreadEvent &event) {
            wxEndBusyCursor();
            auto s = "Enter password for " + this->username_ + "@" + this->host_ + ":" + to_string(this->port_);
            auto passwd = wxGetPasswordFromUser(s, "Sftpgui", wxEmptyString, this);
            this->sftp_thread_channel_->Put(SftpThreadCmdPassword{passwd.ToStdString(wxMBConvUTF8())});
            if (!wxIsBusy()) {
                wxBeginBusyCursor();
            }
        }, ID_SFTP_THREAD_RESPONSE_NEED_PASSWD);

        // Sftp thread will trigger this callback after successfully getting a directory list.
        this->Bind(wxEVT_THREAD, [&](wxThreadEvent &event) {
            auto r = event.GetPayload<SftpThreadResponseGetDir>();
            this->current_dir_list_ = r.dir_list;
            this->current_dir_ = r.dir;
            this->path_text_ctrl_->SetValue(r.dir);
            this->SortAndPopulateDir();
            this->RecallSelected();
            if (this->latest_interesting_status_.empty()) {
                auto d = wxDateTime::Now().FormatISOCombined();
                this->latest_interesting_status_ = "Refreshed dir list at " + d + ".";
            }
            this->SetIdleStatusText();
            wxEndBusyCursor();
        }, ID_SFTP_THREAD_RESPONSE_GET_DIR);

        // Sftp thread will trigger this callback after successfully downloading a file.
        this->Bind(wxEVT_THREAD, [&](wxThreadEvent &event) {
            auto r = event.GetPayload<SftpThreadResponseDownload>();

            // Is remote_path already a key in opened_files_local_?
            if (this->opened_files_local_.find(r.remote_path) != this->opened_files_local_.end()) {
                // Previously downloaded, so just update the modified time.
                this->opened_files_local_[r.remote_path].modified = last_write_time(r.local_path);
            } else {
                OpenedFile f;
                f.local_path = r.local_path;
                f.remote_path = r.remote_path;
                f.modified = last_write_time(r.local_path);
                this->opened_files_local_[r.remote_path] = f;
            }

            string editor = string(this->config_->Read("/editor", ""));
            wxExecute(wxString::FromUTF8(editor + " \"" + r.local_path + "\""), wxEXEC_ASYNC);

            string d = string(wxDateTime::Now().FormatISOCombined());
            this->latest_interesting_status_ = "Downloaded " + r.remote_path + " at " + d + ".";
            this->RefreshDir(this->current_dir_, true);
        }, ID_SFTP_THREAD_RESPONSE_DOWNLOAD);

        // Sftp thread will trigger this callback after successfully uploading a file.
        this->Bind(wxEVT_THREAD, [&](wxThreadEvent &event) {
            auto r = event.GetPayload<SftpThreadResponseUpload>();

            // TODO(allan): this doesnt catch if a file gets written again after the upload starts but before it completes
            auto local_path = this->opened_files_local_[r.remote_path].local_path;
            this->opened_files_local_[r.remote_path].modified = last_write_time(local_path);
            this->opened_files_local_[r.remote_path].upload_requested = false;

            string d = string(wxDateTime::Now().FormatISOCombined());
            this->latest_interesting_status_ = "Uploaded " + r.remote_path + " at " + d + ".";
            this->RefreshDir(this->current_dir_, true);
        }, ID_SFTP_THREAD_RESPONSE_UPLOAD);

        // Sftp thread will trigger this callback on general errors while downloading a file.
        this->Bind(wxEVT_THREAD, [&](wxThreadEvent &event) {
            auto r = event.GetPayload<SftpThreadResponseFileError>();
            auto s = wxString::FromUTF8("Failed to download " + r.remote_path);
            wxMessageDialog dialog(this, s, "Sftpgui Error", wxYES_NO | wxICON_ERROR | wxCENTER);
            dialog.SetYesNoLabels("Retry", "Ignore");
            if (dialog.ShowModal() == wxID_YES) {
                this->DownloadFile(r.remote_path);
            } else {
                // User requested to ignore this download failure.
                this->SetStatusText(s);
                wxEndBusyCursor();
            }
        }, ID_SFTP_THREAD_RESPONSE_DOWNLOAD_FAILED);

        // Sftp thread will trigger this callback on permission errors while downloading a file.
        this->Bind(wxEVT_THREAD, [&](wxThreadEvent &event) {
            auto r = event.GetPayload<SftpThreadResponseFileError>();
            auto s = wxString::FromUTF8("Permission denied when downloading " + r.remote_path);
            wxMessageDialog dialog(this, s, "Sftpgui Error", wxYES_NO | wxICON_ERROR | wxCENTER);
            dialog.SetYesNoLabels("Retry", "Ignore");
            if (dialog.ShowModal() == wxID_YES) {
                this->DownloadFile(r.remote_path);
            } else {
                // User requested to ignore this download failure.
                this->SetStatusText(s);
                wxEndBusyCursor();
            }
        }, ID_SFTP_THREAD_RESPONSE_DOWNLOAD_FAILED_PERMISSION);

        // Sftp thread will trigger this callback on general errors while uploading a file.
        this->Bind(wxEVT_THREAD, [&](wxThreadEvent &event) {
            auto r = event.GetPayload<SftpThreadResponseFileError>();
            auto s = wxString::FromUTF8("Failed to upload " + r.remote_path);
            wxMessageDialog dialog(this, s, "Sftpgui Error", wxYES_NO | wxICON_ERROR | wxCENTER);
            dialog.SetYesNoLabels("Retry", "Ignore");
            if (dialog.ShowModal() == wxID_YES) {
                this->UploadWatchedFile(r.remote_path);
            } else {
                // User requested to ignore this upload failure.
                auto local_path = this->opened_files_local_[r.remote_path].local_path;
                this->opened_files_local_[r.remote_path].modified = last_write_time(local_path);
                this->opened_files_local_[r.remote_path].upload_requested = false;
                this->SetStatusText(s);
                wxEndBusyCursor();
            }
        }, ID_SFTP_THREAD_RESPONSE_UPLOAD_FAILED);

        // Sftp thread will trigger this callback on permission errors while uploading a file.
        this->Bind(wxEVT_THREAD, [&](wxThreadEvent &event) {
            auto r = event.GetPayload<SftpThreadResponseFileError>();
            auto s = wxString::FromUTF8("Permission denied when uploading " + r.remote_path);
            wxMessageDialog dialog(this, s, "Sftpgui Error", wxYES_NO | wxICON_ERROR | wxCENTER);
            dialog.SetYesNoLabels("Retry", "Ignore");
            if (dialog.ShowModal() == wxID_YES) {
                this->UploadWatchedFile(r.remote_path);
            } else {
                // User requested to ignore this upload failure.
                auto local_path = this->opened_files_local_[r.remote_path].local_path;
                this->opened_files_local_[r.remote_path].modified = last_write_time(local_path);
                this->opened_files_local_[r.remote_path].upload_requested = false;
                this->SetStatusText(s);
                wxEndBusyCursor();
            }
        }, ID_SFTP_THREAD_RESPONSE_UPLOAD_FAILED_PERMISSION);

        // Sftp thread will trigger this callback on disk space errors while uploading a file.
        this->Bind(wxEVT_THREAD, [&](wxThreadEvent &event) {
            auto r = event.GetPayload<SftpThreadResponseFileError>();
            auto s = wxString::FromUTF8("Insufficient disk space failure while uploading " + r.remote_path);
            wxMessageDialog dialog(this, s, "Sftpgui Error", wxYES_NO | wxICON_ERROR | wxCENTER);
            dialog.SetYesNoLabels("Retry", "Ignore");
            if (dialog.ShowModal() == wxID_YES) {
                this->UploadWatchedFile(r.remote_path);
            } else {
                // User requested to ignore this upload failure.
                auto local_path = this->opened_files_local_[r.remote_path].local_path;
                this->opened_files_local_[r.remote_path].modified = last_write_time(local_path);
                this->opened_files_local_[r.remote_path].upload_requested = false;
                this->SetStatusText(s);
                wxEndBusyCursor();
            }
        }, ID_SFTP_THREAD_RESPONSE_UPLOAD_FAILED_SPACE);

        // Sftp thread will trigger this callback on disk space errors while listing a directory.
        this->Bind(wxEVT_THREAD, [&](wxThreadEvent &event) {
            auto r = event.GetPayload<SftpThreadResponseFileError>();

            // Make a dummy parent dir entry to make it easy to get back to the parent dir.
            if (this->current_dir_list_.size() == 0) {
                DirEntry parent_dir_entry;
                parent_dir_entry.name_ = "..";
                parent_dir_entry.isDir_ = true;
                this->dir_list_ctrl_->Refresh(vector<DirEntry>{parent_dir_entry});
            }

            auto s = wxString::FromUTF8("Permission denied while listing directory " + r.remote_path);
            wxMessageDialog dialog(this, s, "Sftpgui Error", wxOK | wxICON_ERROR | wxCENTER);
            dialog.ShowModal();
            this->SetStatusText(s);
            wxEndBusyCursor();
        }, ID_SFTP_THREAD_RESPONSE_DIR_LIST_FAILED);

        // Sftp thread will trigger this callback when a file or directory was not found.
        this->Bind(wxEVT_THREAD, [&](wxThreadEvent &event) {
            auto r = event.GetPayload<SftpThreadResponseFileError>();

            // Make a dummy parent dir entry to make it easy to get back to the parent dir.
            if (this->current_dir_list_.size() == 0) {
                DirEntry parent_dir_entry;
                parent_dir_entry.name_ = "..";
                parent_dir_entry.isDir_ = true;
                this->dir_list_ctrl_->Refresh(vector<DirEntry>{parent_dir_entry});
            }

            auto s = wxString::FromUTF8("File or directory not found: " + r.remote_path);
            wxMessageDialog dialog(this, s, "Sftpgui Error", wxOK | wxICON_ERROR | wxCENTER);
            dialog.ShowModal();
            this->SetStatusText(s);
            wxEndBusyCursor();
        }, ID_SFTP_THREAD_RESPONSE_FILE_NOT_FOUND);

        // Sftp thread will trigger this callback on an error that requires us to reconnect.
        this->Bind(wxEVT_THREAD, [&](wxThreadEvent &event) {
            this->RequestUserAttention(wxUSER_ATTENTION_ERROR);
            auto r = event.GetPayload<SftpThreadResponseError>();
            auto error = prettifySentence(r.error);
            if (!wxIsBusy()) {
                wxBeginBusyCursor();
            }

            this->reconnect_timer_error_ = error;
            this->SetStatusText(wxString::FromUTF8(error + " Reconnecting in 5 seconds..."));
            this->reconnect_timer_countdown_ = 5 - 1;
            this->reconnect_timer_.Start(1000);
        }, ID_SFTP_THREAD_RESPONSE_ERROR_CONNECTION);

        // Sftp thread will trigger this callback on general errors.
        this->Bind(wxEVT_THREAD, [&](wxThreadEvent &event) {
            auto r = event.GetPayload<SftpThreadResponseError>();
            auto s = wxString::FromUTF8(prettifySentence(r.error));
            wxMessageDialog dialog(this, s, "Sftpgui Error", wxOK | wxICON_ERROR | wxCENTER);
            this->Close();
        }, ID_SFTP_THREAD_RESPONSE_ERROR);
    }

    void SetIdleStatusText() {
        string s = to_string(this->current_dir_list_.size()) + " items";
        if (!this->latest_interesting_status_.empty()) {
            s += ". " + this->latest_interesting_status_;
        }
        this->SetStatusText(wxString::FromUTF8(s));
    }

    void UploadWatchedFile(string remote_path) {
        OpenedFile f = this->opened_files_local_[remote_path];
        this->sftp_thread_channel_->Put(SftpThreadCmdUpload{f.local_path, f.remote_path});
        if (!wxIsBusy()) {
            wxBeginBusyCursor();
        }
        this->opened_files_local_[f.remote_path].upload_requested = true;
        this->SetStatusText(wxString::FromUTF8("Uploading " + f.remote_path + " ..."));
    }

    void OnFileWatcherTimer(const wxTimerEvent &event) {
        for (auto o : this->opened_files_local_) {
            OpenedFile f = o.second;
            if (!f.upload_requested && last_write_time(f.local_path) > f.modified) {
                this->UploadWatchedFile(f.remote_path);
            }
        }
    }

    void RememberSelected() {
        this->stored_highlighted_ = this->current_dir_list_[this->dir_list_ctrl_->GetHighlighted()].name_;
        this->stored_selected_.clear();
        auto r = this->dir_list_ctrl_->GetSelected();
        for (int i = 0; i < r.size(); ++i) {
            this->stored_selected_.insert(this->current_dir_list_[r[i]].name_);
        }
    }

    void RecallSelected() {
        int highlighted = 0;
        vector<int> selected;
        for (int i = 0; i < this->current_dir_list_.size(); ++i) {
            if (this->stored_selected_.find(this->current_dir_list_[i].name_) != this->stored_selected_.end()) {
                selected.push_back(i);
            }
            if (this->current_dir_list_[i].name_ == this->stored_highlighted_) {
                highlighted = i;
            }
        }
        this->dir_list_ctrl_->SetHighlighted(highlighted);
        this->dir_list_ctrl_->SetSelected(selected);
    }

    void RefreshDir(string remote_path, bool preserve_selection) {
        if (!wxIsBusy()) {
            wxBeginBusyCursor();
        }

        this->SetStatusText("Retrieving directory list...");

        if (preserve_selection) {
            // TODO(allan): what if another RefreshDir happens before the current one is complete?
            this->RememberSelected();
        } else {
            this->stored_selected_.clear();
            this->stored_highlighted_ = "";
        }

        this->sftp_thread_channel_->Put(SftpThreadCmdGetDir{remote_path});
    }

    void SortAndPopulateDir() {
        auto cmp = [&](const DirEntry &a, const DirEntry &b) {
            if (a.name_ == "..") { return true; }
            if (b.name_ == "..") { return false; }
            if (a.isDir_ && !b.isDir_) { return true; }
            if (!a.isDir_ && b.isDir_) { return false; }

            string a_val, b_val;
            if (this->sort_column_ == 1) {
                if (this->sort_desc_) {
                    return a.size_ < b.size_;
                }
                return a.size_ > b.size_;
            } else if (this->sort_column_ == 2) {
                if (this->sort_desc_) {
                    return a.modified_ < b.modified_;
                }
                return a.modified_ > b.modified_;
            } else if (this->sort_column_ == 3) {
                if (this->sort_desc_) {
                    return a.mode_str_ < b.mode_str_;
                }
                return a.mode_str_ > b.mode_str_;
            } else if (this->sort_column_ == 4) {
                if (this->sort_desc_) {
                    return a.owner_ < b.owner_;
                }
                return a.owner_ > b.owner_;
            } else if (this->sort_column_ == 5) {
                if (this->sort_desc_) {
                    return a.group_ < b.group_;
                }
                return a.group_ > b.group_;
            }

            // Assume sort_column == 0.
            if (a.name_.length() > 0 && b.name_.length() > 0 && a.name_[0] == '.' &&
                b.name_[0] != '.') { return true; }
            if (a.name_.length() > 0 && b.name_.length() > 0 && a.name_[0] != '.' &&
                b.name_[0] == '.') { return false; }
            if (this->sort_desc_) {
                return a.name_ < b.name_;
            }
            return a.name_ > b.name_;
        };
        sort(this->current_dir_list_.begin(), this->current_dir_list_.end(), cmp);

        this->dir_list_ctrl_->Refresh(this->current_dir_list_);
    }

    void DownloadFile(string remote_path) {
        string editor = string(this->config_->Read("/editor", ""));
        if (editor.empty()) {
            string msg = "No text editor configured. Set one in Preferences.";
            wxMessageBox(msg, "Notice", wxOK | wxICON_INFORMATION, this);
            return;
        }

        remote_path = normalize_path(remote_path);
        string local_tmp = string(wxStandardPaths::Get().GetTempDir());
        string conn_str = this->username_ + "@" + this->host_ + "_" + to_string(this->port_);
        string local_path = normalize_path(local_tmp + "/sftpgui/" + conn_str + "/" + remote_path);
        string local_dir = normalize_path(local_path + "/..");
        // TODO(allan): restrict permissions
        // TODO(allan): handle local file creation error seperately from a connection errors
        create_directories(local_dir);

        this->sftp_thread_channel_->Put(SftpThreadCmdDownload{local_path, remote_path});
        if (!wxIsBusy()) {
            wxBeginBusyCursor();
        }
    }
};


class SftpguiApp : public wxApp {
    string host_;
    string username_;
    int port_ = 22;

public:
    bool OnInit() {
        try {
            if (!wxApp::OnInit())
                return false;


            auto es = wxEmptyString;
            wxFileConfig *config = new wxFileConfig("sftpgui", es, es, es, wxCONFIG_USE_LOCAL_FILE);
            config->EnableAutoSave();
            config->SetRecordDefaults();
            wxConfigBase::Set(config);

            if (host_.empty()) {
                wxTextEntryDialog dialog(0,
                                         "Enter remote host.\n"
                                         "Format: [username@]host:port\n"
                                         "Defaults to current local username and port 22 if not specified.",
                                         "Sftpgui");
                if (dialog.ShowModal() == wxID_CANCEL) {
                    return false;
                }
                if (!this->ParseHost(string(dialog.GetValue()))) {
                    return false;
                }
            }

            wxFrame *frame = new SftpguiFrame(this->username_, this->host_, this->port_, config);
            frame->Show();
        } catch (...) {
            showException();
            return false;
        }

        return true;
    }

    virtual void OnInitCmdLine(wxCmdLineParser &parser) {  // NOLINT: wxWidgets legacy
        parser.SetSwitchChars(wxT("-"));
        parser.AddParam("[user@]host:port", wxCMD_LINE_VAL_STRING, wxCMD_LINE_PARAM_OPTIONAL);
        parser.AddSwitch("h", "help", "displays help", wxCMD_LINE_OPTION_HELP);
    }

    virtual bool OnCmdLineParsed(wxCmdLineParser &parser) {  // NOLINT: wxWidgets legacy
        if (parser.GetParamCount() > 0) {
            if (!this->ParseHost(string(parser.GetParam(0)))) {
                return false;
            }
        }

        return true;
    }

    virtual bool OnExceptionInMainLoop() {
        showException();
        return false;
    }

private:
    bool ParseHost(string host) {
        this->username_ = wxGetUserId();
        this->host_ = host;
        this->port_ = 22;

#ifdef __WXMSW__
        transform(this->username_.begin(), this->username_.end(), this->username_.begin(), ::tolower);
#endif

        if (this->host_.find("@") != string::npos) {
            int i = this->host_.find("@");
            this->username_ = this->host_.substr(0, i);
            this->host_ = this->host_.substr(i + 1);
        }

        if (this->host_.find(":") != string::npos) {
            int i = this->host_.find(":");

            string ps = string(this->host_.substr(i + 1));
            if (!std::all_of(ps.begin(), ps.end(), ::isdigit)) {
                wxLogFatalError("non-digit port number");
                return false;
            }
            this->port_ = stoi(string(ps));
            if (!(0 < this->port_ && this->port_ < 65536)) {
                wxLogFatalError("invalid port number");
                return false;
            }

            this->host_ = this->host_.substr(0, i);
        }

        return true;
    }
};

IMPLEMENT_APP(SftpguiApp)
