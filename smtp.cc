#include <cstdlib>
#include <cstdio>
#include <getopt.h>
#include <unistd.h>
#include <iostream>
#include <string>
#include <cerrno>
#include <cstring>
#include <csignal>
#include <pthread.h>
#include <vector>

#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include <sys/stat.h>

#include <fcntl.h>
#include <ctime>
#include <unordered_map>
#include <sys/file.h>   // flock
#include <algorithm>
#include <unordered_set>
#include <atomic>

using namespace std;

static string g_mbox_root;   // where <user>.mbox files live

static pthread_mutex_t g_lockmap_mu = PTHREAD_MUTEX_INITIALIZER;
static std::unordered_map<std::string, pthread_mutex_t*> g_mbox_mu;

static std::atomic<bool> g_shutting_down(false);
static int g_listen_fd = -1;

static pthread_mutex_t g_conns_mu = PTHREAD_MUTEX_INITIALIZER;
static std::unordered_set<int> g_conns;

// Returns a copy of s with leading spaces and tabs removed.
static string ltrim_copy (string s) {
  size_t i = 0;
  while (i < s.size() && (s[i] == ' ' || s[i] == '\t')) i++;
  s.erase(0, i);
  return s;
}

// Returns a copy of s with trailing spaces and tabs removed.
static string rtrim_copy (string s) {
  while (!s.empty() && (s.back() == ' ' || s.back() == '\t')) s.pop_back();
  return s;
}

// Adds connection fd to the global set of open connections (for shutdown cleanup).
static void register_conn(int fd) {
  pthread_mutex_lock(&g_conns_mu);
  g_conns.insert(fd);
  pthread_mutex_unlock(&g_conns_mu);
}

// Removes connection fd from the global set of open connections.
static void unregister_conn(int fd) {
  pthread_mutex_lock(&g_conns_mu);
  g_conns.erase(fd);
  pthread_mutex_unlock(&g_conns_mu);
}

// SIGINT handler: sets shutdown flag and closes listen socket so accept() unblocks.
static void on_sigint(int) {
  g_shutting_down.store(true);
  if (g_listen_fd >= 0) close(g_listen_fd);  // causes accept() to fail/unblock
}

// If s_in is of the form <...>, extracts the trimmed content between brackets into inside; returns true on success.
static bool extract_angle_addr(const string& s_in, string& inside) {
  string s = ltrim_copy(rtrim_copy(s_in));
  if (s.size() < 2) return false;
  if (s.front() != '<' || s.back() != '>') return false;
  inside = s.substr(1, s.size() - 2);
  inside = ltrim_copy(rtrim_copy(inside));
  return !inside.empty();
}

// Returns (creating if needed) the per-mailbox mutex for mbox_path; used to serialize writes to the same mailbox across threads.
static pthread_mutex_t* get_mbox_mutex(const std::string& mbox_path) {
  pthread_mutex_lock(&g_lockmap_mu);

  auto it = g_mbox_mu.find(mbox_path);
  if (it == g_mbox_mu.end()) {
    auto* m = new pthread_mutex_t;
    pthread_mutex_init(m, nullptr);
    g_mbox_mu[mbox_path] = m;
    it = g_mbox_mu.find(mbox_path);
  }

  pthread_mutex_unlock(&g_lockmap_mu);
  return it->second;
}

// Sends exactly len bytes from buf to socket fd (handles partial sends and EINTR); returns false on error.
static bool write_all(int fd, const char* buf, size_t len) {
  size_t sent = 0;
  while (sent < len) {
    ssize_t n = send(fd, buf + sent, len - sent, 0);
    if (n < 0) {
      if (errno == EINTR) continue;
      return false;
    }
    if (n == 0) return false;
    sent += (size_t)n;
  }
  return true;
}

// Writes exactly len bytes from buff to file fd (handles partial writes and EINTR); returns false on error.
static bool write_all_file (int fd, const char* buff, size_t len) {
  size_t off = 0;
  while (off < len) {
    ssize_t n = write(fd, buff + off, len - off);   // <-- ssize_t
    if (n < 0) {
      if (errno == EINTR) continue;
      return false;
    }
    if (n == 0) return false;
    off += (size_t)n;
  }
  return true;
}

// Overload: sends the entire string s to socket fd.
static bool write_all(int fd, const string& s) {
  return write_all(fd, s.c_str(), s.size());
}

// Appends one message to the mbox file at mbox_path: acquires per-mailbox mutex and file lock, writes mbox "From ..." line plus data_text, then releases lock and closes file.
static bool append_to_mbox(const string& mbox_path,
  const string& mail_from,
  const string& data_text,
  bool verbose)
{
// 1) mutex: stop 2 SMTP threads writing same mailbox at once
pthread_mutex_t* mu = get_mbox_mutex(mbox_path);
pthread_mutex_lock(mu);

// 2) open file (O_CREAT so first message can create mailbox file)
int fd = open(mbox_path.c_str(), O_WRONLY | O_APPEND | O_CREAT, 0666);
if (fd < 0) {
if (verbose) cerr << "open failed: " << strerror(errno) << " path=" << mbox_path << "\n";
pthread_mutex_unlock(mu);
return false;
}

// 3) flock: stop SMTP process vs POP3 process
if (flock(fd, LOCK_EX) != 0) {
if (verbose) cerr << "flock failed: " << strerror(errno) << " path=" << mbox_path << "\n";
close(fd);
pthread_mutex_unlock(mu);
return false;
}

// 4) build “From ... date” header line
time_t now = time(nullptr);
char tbuf[64];
ctime_r(&now, tbuf);
size_t L = strlen(tbuf);
if (L > 0 && tbuf[L-1] == '\n') tbuf[L-1] = '\0';

string header = "From " + mail_from + " " + string(tbuf) + "\n";

// 5) write header + message + blank line
bool ok = true;
ok = ok && write_all_file(fd, header.c_str(), header.size());
ok = ok && write_all_file(fd, data_text.c_str(), data_text.size());
ok = ok && write_all_file(fd, "\n", 1);

// 6) unlock in reverse order
flock(fd, LOCK_UN);
close(fd);
pthread_mutex_unlock(mu);

return ok;
}

// Reads available data from socket fd and appends it to buf; returns false on client close or fatal error.
static bool recv_into_buffer(int fd, string &buf) {
  char tmp[2048];
  while (true) {
    ssize_t n = recv(fd, tmp, sizeof(tmp), 0);
    if (n > 0) {
      buf.append(tmp, tmp + n);
      return true;
    }
    if (n == 0) return false;           // client closed
    if (errno == EINTR) continue;       // try again
    return false;                       // other error
  }
}

// If buf contains a full line ending in CRLF, removes that line (without CRLF) into line and returns true; otherwise returns false and leaves buf unchanged.
static bool pop_crlf_line(string &buf, string &line) {
  size_t pos = buf.find("\r\n");
  if (pos == string::npos) return false;
  line = buf.substr(0, pos);
  buf.erase(0, pos + 2);
  return true;
}

// Creates a TCP listening socket bound to the given port (with SO_REUSEADDR); returns its fd or exits on failure.
static int make_listen_socket(int port) {
  int fd = socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0) {
    cerr << "socket() failed: " << strerror(errno) << "\n";
    exit(1);
  }

  int yes = 1;
  if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes)) < 0) {
    cerr << "setsockopt() failed: " << strerror(errno) << "\n";
    exit(1);
  }

  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_ANY);
  addr.sin_port = htons((uint16_t)port);

  if (bind(fd, (sockaddr*)&addr, sizeof(addr)) < 0) {
    cerr << "bind() failed: " << strerror(errno) << "\n";
    exit(1);
  }

  if (listen(fd, 100) < 0) {
    cerr << "listen() failed: " << strerror(errno) << "\n";
    exit(1);
  }

  return fd;
}



// If s is a valid user@localhost address (optionally in <...>), extracts the local part into user and returns true; otherwise returns false.
static bool parse_user_at_localhost (string s, string &user) {
  s = ltrim_copy(rtrim_copy(s));
  if (!s.empty() && s.front() == '<' && s.back() == '>' && s.size() >= 2) {
    s = s.substr(1, s.size()-2);
    s = ltrim_copy(rtrim_copy(s));
  }
  size_t at = s.find('@');
  if (at == string::npos) return false;
  string local = s.substr(0, at);
  string domain = s.substr(at+1);

  if (local.empty() || domain.empty()) return false;
  for (char &c : domain) c = (char)toupper((unsigned char)c);
  if (domain != "LOCALHOST") return false;

  if (local.find('/') != string::npos || local.find('\\') != string::npos) return false;
  user = local;
  return true;
}

// Returns true if path exists and is a regular file (used to check that a mailbox file exists).
static bool is_regular_file(const string& path) {
  struct stat st{};
  return (stat(path.c_str(), &st) == 0) && S_ISREG(st.st_mode);
}


// Runs the SMTP session for one client: sends greeting, then handles HELO, MAIL FROM, RCPT TO, DATA, RSET, NOOP, QUIT (rejects EHLO); buffers input by line and appends delivered messages to the appropriate mbox files.
static void handle_client_smtp(int conn_fd, bool verbose) {
  bool in_data = false;
  string data_text;

 

  bool helo_ok = false;
  // 1) Greeting
  const string greet = "220 localhost Service ready\r\n";
  if (verbose) cerr << "[" << conn_fd << "] S: 220 localhost Service ready\n";
  if (!write_all(conn_fd, greet)) return;

  // 2) Per-connection input buffer
  string buf;
  bool have_mail_from = false;
  string mail_from;
  vector<string> rcpt_users;

  auto reset_txn = [&]() {
    in_data = false;
    data_text.clear();
    have_mail_from = false;
    mail_from.clear();
    rcpt_users.clear();
  };
  

  auto send_response = [&] (const string &resp_line) {
    if (verbose) cerr << "[" << conn_fd << "] S: " << resp_line << "\n";
    return write_all(conn_fd, resp_line + "\r\n");
  };

  // 3) Loop forever: read more data, and process full lines ending in \r\n
  while (true) {
    // Process any complete lines already in buffer
    string line;
    while (pop_crlf_line(buf, line)) {
      if (verbose) cerr << "[" << conn_fd << "] C: " << line << "\n";

      if (in_data) {
        if (line == ".") {
          bool ok_write = true;
          for (const string& user : rcpt_users) {
            string mbox_path = g_mbox_root + "/" + user + ".mbox";
            if (!append_to_mbox(mbox_path, mail_from, data_text, verbose)) {
              ok_write = false;
            }
          }
          reset_txn();

          if (!ok_write) {
            send_response("451 Requested action aborted: local error in processing");
          } else {
            send_response("250 OK");
          }
          continue;
        }
        if (!line.empty() && line[0] == '.') line.erase(0, 1);

        data_text += line;
        data_text += "\n";
        continue;
      }

      // Extract command word (first token)
      string cmd, rest, resp;
      size_t sp = line.find(' ');
      if (sp == string::npos) {
        cmd = line;
        rest = "";
      } else {
        cmd = line.substr(0, sp);
        rest = line.substr(sp+1);
      }
      rest = ltrim_copy(rtrim_copy(rest));

      // SMTP commands are case-insensitive
      for (char &c : cmd) c = (char)toupper((unsigned char)c);

     

      if (cmd == "EHLO") {
        send_response("500 Syntax error, command unrecognized");
        continue;
      }

      if (cmd == "HELO") {
        if (rest.empty()) {
          send_response("501 Syntax error in parameters or arguments");
        } else {
          helo_ok = true;
          send_response("250 OK");
        }
        continue;
      }

      if (cmd == "QUIT") {
        send_response("221 Bye");
        return; // thread will close socket
      } 

      
      if (!helo_ok) {
        send_response("503 Bad sequence of commands");
        continue;
      }
      if (cmd == "NOOP") {
        send_response("250 OK");
        continue;
      }
      
      if (cmd == "RSET") {
        reset_txn();
        send_response("250 OK");
        continue;
      }

     

      // Mail From:
      if (cmd == "MAIL") {
        string up = rest;
        for (char &c : up) c = (char)toupper((unsigned char)c);
      
        if (up.rfind("FROM:", 0) != 0) {
          send_response("501 Syntax error: MAIL FROM:<address>");
          continue;
        }
      
        string after = ltrim_copy(rest.substr(5));  // after "FROM:"
        string addr_inside;
        if (!extract_angle_addr(after, addr_inside)) {
          send_response("501 Syntax error: MAIL FROM:<address>");
          continue;
        }
      
        mail_from = addr_inside;
        have_mail_from = true;
        rcpt_users.clear();
        send_response("250 OK");
        continue;
      }
    
      // RCPT To:
      if (cmd == "RCPT") {
        if (!have_mail_from) {
          send_response("503 Bad sequence of commands");
          continue;
        }
      
        string up = rest;
        for (char& c : up) c = (char)toupper((unsigned char)c);
      
        if (up.rfind("TO:", 0) != 0) {
          send_response("501 Syntax error: RCPT TO:<address>");
          continue;
        }
      
        string after = ltrim_copy(rest.substr(3));  // after "TO:"
        string addr_inside;
        if (!extract_angle_addr(after, addr_inside)) {
          send_response("501 Syntax error: RCPT TO:<address>");
          continue;
        }
      
        // addr_inside must be user@localhost (and file must exist)
        string user;
        if (!parse_user_at_localhost(addr_inside, user)) {
          send_response("550 no such user here");
          continue;
        }
      
        string mbox_path = g_mbox_root + "/" + user + ".mbox";
        if (!is_regular_file(mbox_path)) {
          send_response("550 no such user here");
          continue;
        }
      
        // Dedupe (don’t deliver same message twice to same mailbox)
        if (find(rcpt_users.begin(), rcpt_users.end(), user) == rcpt_users.end()) {
          rcpt_users.push_back(user);
        }
      
        send_response("250 OK");
        continue;
      }

      if (cmd == "DATA") {
        if (!have_mail_from || rcpt_users.empty()) {
          send_response("503 Bad sequence of commands");
          continue;
        }
        in_data = true;
        data_text.clear();
        send_response("354 End data with <CRLF>.<CRLF>");
        continue;
      }
      
     
      send_response("500 Syntax error, command unrecognized");
      continue;
    }

    // No full line yet -> read more bytes
    if (!recv_into_buffer(conn_fd, buf)) {
      if (verbose) cerr << "[" << conn_fd << "] Connection closed\n";
      return;
    }
  }
}



// Arguments passed to each SMTP worker thread.
struct ThreadArgs {
  int fd;
  bool verbose;
};

// Worker thread entry: runs handle_client_smtp, then unregisters the connection and closes the socket.
static void* client_thread(void* arg) {
  ThreadArgs* a = (ThreadArgs*)arg;
  int fd = a->fd;
  bool verbose = a->verbose;
  delete a;

  handle_client_smtp(fd, verbose);
  unregister_conn(fd);
  close(fd);
  if (verbose) cerr << "[" << fd << "] Connection closed\n";
  return nullptr;
}

// Parses options and maildir, creates the listening socket, accepts connections and spawns worker threads; on shutdown sends 421 to all tracked connections and closes them.
int main(int argc, char* argv[]) {
  // Don’t let SIGPIPE kill the server when writing to a closed socket.
  signal(SIGPIPE, SIG_IGN);
  signal(SIGINT, on_sigint);

  int opt;
  int port = 2500;     // SMTP default
  bool verbose = false;

  while ((opt = getopt(argc, argv, "p:va")) != -1) {
    switch (opt) {
      case 'p': port = atoi(optarg); break;
      case 'v': verbose = true; break;
      case 'a':
        cerr << "Full name: Anshu Gupta\nSEAS login: anshuykg\n";
        return 0;
      default:
        cerr << "Usage: " << argv[0] << " [-p port] [-v] [-a] <maildir>\n";
        return 1;
    }
  }

  // positional arg: maildir
  if (optind >= argc) {
    cerr << "Usage: " << argv[0] << " [-p port] [-v] [-a] <maildir>\n";
    return 1;
  }
  string maildir = argv[optind];

  // check maildir exists (for now just ensure it’s a directory)
  struct stat st{};
  if (stat(maildir.c_str(), &st) != 0 || !S_ISDIR(st.st_mode)) {
    cerr << "Error: maildir is not a directory: " << maildir << "\n";
    return 1;
  }

  g_mbox_root = maildir;
  int listen_fd = make_listen_socket(port);
  g_listen_fd = listen_fd;
  if (verbose) cerr << "Listening on port " << port << " (maildir=" << maildir << ")\n";

  while (true) {
    sockaddr_in client{};
    socklen_t len = sizeof(client);

    int conn_fd = accept(listen_fd, (sockaddr*)&client, &len);
    
    if (conn_fd < 0) {
      if (errno == EINTR) continue;
      if (g_shutting_down.load()) break;
      cerr << "accept() failed: " << strerror(errno) << "\n";
      continue;
    }
    register_conn(conn_fd);

    if (verbose) cerr << "[" << conn_fd << "] New connection\n";

    pthread_t tid;
    ThreadArgs* args = new ThreadArgs{conn_fd, verbose};
    int rc = pthread_create(&tid, nullptr, client_thread, args);
    if (rc != 0) {
      cerr << "pthread_create failed: " << strerror(rc) << "\n";
      close(conn_fd);
      delete args;
      continue;
    }
    pthread_detach(tid);
  }
  pthread_mutex_lock(&g_conns_mu);
for (int fd : g_conns) {
  const char* msg = "421 Server shutting down\r\n";
  send(fd, msg, strlen(msg), 0);
  shutdown(fd, SHUT_RDWR);
  close(fd);
}
g_conns.clear();
pthread_mutex_unlock(&g_conns_mu);
}
