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

#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include <sys/stat.h>

#include <vector>
#include <fstream>
#include <sstream>
#include <cctype>

#include <openssl/md5.h>
#include <iomanip>   // for setw, setfill

#include <sys/file.h>   // flock
#include <unordered_map>

#include <unordered_set>
#include <atomic>
#include <fcntl.h>
using namespace std;

static pthread_mutex_t g_lockmap_mu = PTHREAD_MUTEX_INITIALIZER;
static std::unordered_map<std::string, pthread_mutex_t*> g_mbox_mu;

static std::atomic<bool> g_shutting_down(false);
static int g_listen_fd = -1;

static pthread_mutex_t g_conns_mu = PTHREAD_MUTEX_INITIALIZER;
static std::unordered_set<int> g_conns;

static void register_conn(int fd) {
  pthread_mutex_lock(&g_conns_mu);
  g_conns.insert(fd);
  pthread_mutex_unlock(&g_conns_mu);
}

static void unregister_conn(int fd) {
  pthread_mutex_lock(&g_conns_mu);
  g_conns.erase(fd);
  pthread_mutex_unlock(&g_conns_mu);
}

static void on_sigint(int) {
  g_shutting_down.store(true);
  if (g_listen_fd >= 0) close(g_listen_fd); // unblock accept()
}

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

/* ---------- helpers: write_all / recv buffer / CRLF line ---------- */
static bool write_all(int fd, const char* buf, size_t len) {
  size_t sent = 0;
  while (sent < len) {
    ssize_t n = send(fd, buf + sent, len - sent, 0);
    if (n < 0) { if (errno == EINTR) continue; return false; }
    if (n == 0) return false;
    sent += (size_t)n;
  }
  return true;
}

static bool write_all(int fd, const string& s) { return write_all(fd, s.c_str(), s.size()); }

/* ---------- Message model ---------- */
struct Msg {
  string from_line;  // includes trailing "\n"
  string text;       // message content (LF ok)
  bool deleted;
  Msg() : deleted(false) {}
};

static bool recv_into_buffer(int fd, string &buf) {
  char tmp[2048];
  while (true) {
    ssize_t n = recv(fd, tmp, sizeof(tmp), 0);
    if (n > 0) { buf.append(tmp, tmp + n); return true; }
    if (n == 0) return false;
    if (errno == EINTR) continue;
    return false;
  }
}

/* ---------- commit deletions on QUIT ----------
   For simplicity: rewrite mailbox keeping only undeleted messages.
*/
static bool rewrite_mbox_without_deleted(const string& mbox_path,
  const vector<Msg>& msgs) {
string tmp = mbox_path + ".tmp";
ofstream out(tmp);
if (!out.is_open()) return false;

for (const auto& m : msgs) {
if (m.deleted) continue;
out << m.from_line;   // preserve original
out << m.text;
if (!m.text.empty() && m.text.back() != '\n') out << "\n";
out << "\n";
}
out.close();
if (!out) return false;

if (rename(tmp.c_str(), mbox_path.c_str()) != 0) {
unlink(tmp.c_str());
return false;
}
return true;
}

/* ---------- dot-stuffing + CRLF conversion for POP3 wire ---------- */
static string to_pop3_wire(const string& lf_text) {
  // POP3 requires CRLF line endings.
  // Also dot-stuff any line that begins with '.'.
  string out;
  out.reserve(lf_text.size() + 32);

  // Convert LF to CRLF manually
  size_t start = 0;
  while (start < lf_text.size()) {
    size_t nl = lf_text.find('\n', start);
    string line;
    if (nl == string::npos) {
      line = lf_text.substr(start);
      start = lf_text.size();
    } else {
      line = lf_text.substr(start, nl - start);
      start = nl + 1;
    }

    // Dot-stuff
    if (!line.empty() && line[0] == '.') out.push_back('.');

    out += line;
    out += "\r\n";
  }
  return out;
}

static bool pop_crlf_line(string &buf, string &line) {
  size_t pos = buf.find("\r\n");
  if (pos == string::npos) return false;
  line = buf.substr(0, pos);
  buf.erase(0, pos + 2);
  return true;
}


static bool load_mbox_messages(const string& mbox_path, vector<Msg>& msgs) {
  msgs.clear();
  ifstream in(mbox_path);
  if (!in.is_open()) return false;

  string line;
  bool have_any = false;
  Msg cur;
  bool in_msg = false;

  while (getline(in, line)) {
    // mbox separator
    if (line.rfind("From ", 0) == 0) {
      if (in_msg) { msgs.push_back(cur); cur = Msg(); }
      in_msg = true;
      have_any = true;
      cur.from_line = line + "\n";  // keep it!
      continue;
    }
    if (in_msg) {
      cur.text += line;
      cur.text += "\n";
    }
  }
  if (in_msg) msgs.push_back(cur);

  // It is valid for mailbox to be empty
  if (!have_any) msgs.clear();
  return true;
}


void computeDigest(char *data, int dataLengthBytes, unsigned char *digestBuffer)
{
  /* The digest will be written to digestBuffer, which must be at least MD5_DIGEST_LENGTH bytes long */
  MD5_CTX c;
  MD5_Init(&c);
  MD5_Update(&c, data, dataLengthBytes);
  MD5_Final(digestBuffer, &c);
}

static string md5_hex(const string& s) {
  unsigned char digest[MD5_DIGEST_LENGTH];

  // computeDigest wants (char*, int, unsigned char*)
  computeDigest((char*)s.data(), (int)s.size(), digest);

  ostringstream oss;
  oss << hex << setfill('0');
  for (int i = 0; i < MD5_DIGEST_LENGTH; i++) {
    oss << setw(2) << (int)digest[i];
  }
  return oss.str();
}

struct ThreadArgs {
  int fd;
  bool verbose;
  string maildir;
};
static size_t pop3_octets(const Msg& m) {
  return to_pop3_wire(m.text).size(); // CRLF + dot-stuffing included
}

/* ---------- small string helpers ---------- */
static string trim_copy(string s) {
  // trim left
  size_t i = 0;
  while (i < s.size() && (s[i] == ' ' || s[i] == '\t')) i++;
  s.erase(0, i);
  // trim right
  while (!s.empty() && (s.back() == ' ' || s.back() == '\t')) s.pop_back();
  return s;
}

static bool valid_user(const string& u) {
  if (u.empty()) return false;
  if (u.find('/') != string::npos) return false;
  if (u.find('\\') != string::npos) return false;
  if (u.find("..") != string::npos) return false;
  return true;
}

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
static string upper_copy(string s) {
  for (char &c : s) c = (char)toupper((unsigned char)c);
  return s;
}

static bool is_regular_file(const string& path) {
  struct stat st{};
  return (stat(path.c_str(), &st) == 0) && S_ISREG(st.st_mode);
}

static void handle_client_pop3 (int fd, bool verbose, string maildir) {
  pthread_mutex_t* mbox_mu = nullptr;
  int mbox_fd = -1;

  auto send_line = [&](const std::string& s) -> bool {
    if (verbose) std::cerr << "[" << fd << "] S: " << s << "\n";
    return write_all(fd, s + "\r\n");
  };

  if (!send_line("+OK POP3 ready [localhost]")) return;
  string buf;
  bool authed = false;
  string user;
  vector <Msg> msgs;
  string mbox_path;
  while (true) {
    string line;
       // Need a full CRLF line
       while (!pop_crlf_line(buf, line)) {
        if (!recv_into_buffer(fd, buf)) {
          if (verbose) std::cerr << "[" << fd << "] Connection closed\n";
          return;
        }
      }
      if (verbose) std::cerr << "[" << fd << "] C: " << line << "\n";
      line = trim_copy(line);

       // split: cmd + arg
    std::string cmd, arg;
    size_t sp = line.find(' ');
    if (sp == std::string::npos) { cmd = line; arg = ""; }
    else { cmd = line.substr(0, sp); arg = trim_copy(line.substr(sp + 1)); }
    cmd = upper_copy(cmd);

        // QUIT works anytime
        if (cmd == "QUIT") {
          if (!arg.empty()) { send_line("-ERR"); continue; } // optional strict arg check
        
          if (authed) {
            if (!rewrite_mbox_without_deleted(mbox_path, msgs)) {
              send_line("-ERR could not update mailbox");
            } else {
              send_line("+OK Goodbye!");
            }
        
            if (mbox_fd >= 0) { flock(mbox_fd, LOCK_UN); close(mbox_fd); mbox_fd = -1; }
            if (mbox_mu) { pthread_mutex_unlock(mbox_mu); mbox_mu = nullptr; }
          } else {
            send_line("+OK Goodbye!");
          }
          return;
        }

         // USER
         if (cmd == "USER") {
          if (arg.empty()) { send_line("-ERR missing username"); continue; }
        
          // If already holding a mailbox lock, release it before switching users
          if (mbox_fd >= 0) { flock(mbox_fd, LOCK_UN); close(mbox_fd); mbox_fd = -1; }
          if (mbox_mu) { pthread_mutex_unlock(mbox_mu); mbox_mu = nullptr; }
        
          if (!valid_user(arg)) { send_line("-ERR invalid username"); continue; }
        
          user = arg;
          mbox_path = maildir + "/" + user + ".mbox";
          authed = false;
          msgs.clear();
          send_line("+OK User accepted");
          continue;
        }

        // PASS (very simple for milestone: accept any password, but require that mailbox file exists)
        if (cmd == "PASS") {
          if (user.empty()) { send_line("-ERR USER required"); continue; }

          if (arg != "cis505") {
            send_line("-ERR invalid password");
            user.clear();
            continue;
          }

          if (!is_regular_file(mbox_path)) {
            send_line("-ERR no such mailbox");
            user.clear();
            continue;
          }

            // 1) thread lock for this mailbox
           mbox_mu = get_mbox_mutex(mbox_path);
           pthread_mutex_lock(mbox_mu);

            // 2) open file (keep it open for session)
          mbox_fd = open(mbox_path.c_str(), O_RDWR);
          if (mbox_fd < 0) {
            pthread_mutex_unlock(mbox_mu);
            mbox_mu = nullptr;
            send_line("-ERR could not open mailbox");
            user.clear();
            continue;
          }

           // 3) process lock for this mailbox
          if (flock(mbox_fd, LOCK_EX) != 0) {
            close(mbox_fd);
            mbox_fd = -1;
            pthread_mutex_unlock(mbox_mu);
            mbox_mu = nullptr;
            send_line("-ERR could not lock mailbox");
            user.clear();
            continue;
          }

                    // Now safe to read mailbox
          if (!load_mbox_messages(mbox_path, msgs)) {
            flock(mbox_fd, LOCK_UN);
            close(mbox_fd);
            mbox_fd = -1;
            pthread_mutex_unlock(mbox_mu);
            mbox_mu = nullptr;
            send_line("-ERR could not read mailbox");
            user.clear();
            continue;
          }
    
          authed = true;
          send_line("+OK Mailbox locked and ready");
          continue;
        }
        // Everything else requires auth
    if (!authed) {
      send_line("-ERR authenticate first (USER/PASS)");
      continue;
    }

    // NOOP
    if (cmd == "NOOP") {
      if (!arg.empty()) { send_line("-ERR"); continue; }
      send_line("+OK");
      continue;
    }

    // RSET: undelete
    if (cmd == "RSET") {
      for (auto &m : msgs) m.deleted = false;
      send_line("+OK");
      continue;
    }

    // STAT
    if (cmd == "STAT") {
      if (!arg.empty()) { send_line("-ERR"); continue; } // optional strict
      int count = 0;
      size_t octets = 0;
      for (auto &m : msgs) {
        if (m.deleted) continue;
        count++;
        octets += pop3_octets(m);
      }
      send_line("+OK " + to_string(count) + " " + to_string(octets));
      continue;
    }

        // LIST (all) or LIST n
        if (cmd == "LIST") {
          if (arg.empty()) {
            send_line("+OK scan listing follows");
            for (size_t i = 0; i < msgs.size(); i++) {
              if (msgs[i].deleted) continue;
              send_line(to_string(i + 1) + " " + to_string(pop3_octets(msgs[i])));
            }
            send_line(".");
          } else {
            int n = atoi(arg.c_str());
            if (n <= 0 || (size_t)n > msgs.size() || msgs[n - 1].deleted) {
              send_line("-ERR no such message");
            } else {
              send_line("+OK " + to_string(n) + " " + to_string(pop3_octets(msgs[n - 1])));
            }
          }
          continue;
        }

            // RETR n
    if (cmd == "RETR") {
      int n = atoi(arg.c_str());
      if (n <= 0 || (size_t)n > msgs.size() || msgs[n - 1].deleted) {
        send_line("-ERR no such message");
        continue;
      }

      send_line("+OK message follows");

      std::string wire = to_pop3_wire(msgs[n - 1].text);
      if (!write_all(fd, wire)) return;

      // POP3 message terminator
      if (!write_all(fd, ".\r\n")) return;
      continue;
    }

    if (cmd == "UIDL") {
      if (arg.empty()) {
        send_line("+OK unique-id listing follows");
        for (size_t i = 0; i < msgs.size(); i++) {
          if (msgs[i].deleted) continue;
          send_line(to_string(i + 1) + " " + md5_hex(msgs[i].from_line + msgs[i].text));
        }
        send_line(".");
      } else {
        int n = atoi(arg.c_str());
        if (n <= 0 || (size_t)n > msgs.size() || msgs[n - 1].deleted) {
          send_line("-ERR no such message");
        } else {
          send_line("+OK " + to_string(n) + " " +
                    md5_hex(msgs[n - 1].from_line + msgs[n - 1].text));
        }
      }
      continue;
    }

    if (cmd == "DELE") {
      int n = atoi(arg.c_str());
      if (n <= 0 || (size_t)n > msgs.size() || msgs[n - 1].deleted) {
        send_line("-ERR no such message");
      } else {
        msgs[n - 1].deleted = true;
        send_line("+OK message deleted");
      }
      continue;
    }
    send_line("-ERR Not supported");
continue;

  }

}

static void* client_thread(void* arg) {
  ThreadArgs* a = (ThreadArgs*)arg;
  int fd = a->fd;
  bool verbose = a->verbose;
  string maildir = a->maildir;
  delete a;
  handle_client_pop3(fd, verbose, maildir);
  unregister_conn(fd);
  close(fd);
  if (verbose) cerr << "[" << fd << "] Connection closed\n";
  return nullptr;
}

int main(int argc, char *argv[])
{
  signal(SIGPIPE, SIG_IGN);
signal(SIGINT, on_sigint);
  int port = 11000;
  bool verbose = false;
  int opt;
  while ((opt = getopt(argc, argv, "p:va")) != -1) {
    switch(opt) {
      case 'p': 
      port = atoi(optarg);
      break;
      case 'v': 
      verbose = true;
      break;
      case 'a':
        cerr << "Full name: Anshu Gupta\nSEAS login: anshuykg\n";
        return 0;
      default:
        cerr << "Usage: " << argv[0] << " [-p port] [-v] [-a] <maildir>\n";
        return 1;
    }
  }
  
  if (optind >= argc) {
    cerr << "Usage: " << argv[0] << " [-p port] [-v] [-a] <maildir>\n";
    return 1;
  }
  string maildir = argv[optind];
  if (verbose) {
    cerr << "Port= " << port << " maildir= " << maildir << "\n";
  }
  struct stat st{};
  if (stat(maildir.c_str(), &st) != 0 || !S_ISDIR(st.st_mode)) {
    cerr << "Error: maildir is not a directory: " << maildir << "\n";
    return 1;
  }

  int listen_fd = make_listen_socket(port);
g_listen_fd = listen_fd;
  if (verbose) {
    cerr << "Listening on port " << port << " (maildir=" << maildir << ")\n";
  }
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
    ThreadArgs* args = new ThreadArgs{conn_fd, verbose, maildir};
    int rc = pthread_create(&tid, nullptr, client_thread, args);
    if (rc != 0) {
      cerr << "pthread_create failed: " << strerror(rc) << "\n";
      unregister_conn(conn_fd);
      close(conn_fd);
      delete args;
      continue;
    }
    pthread_detach(tid);
  }
  pthread_mutex_lock(&g_conns_mu);
for (int fd : g_conns) {
  const char* msg = "-ERR Server shutting down\r\n";
  send(fd, msg, strlen(msg), 0);
  shutdown(fd, SHUT_RDWR);
  close(fd);
}
g_conns.clear();
pthread_mutex_unlock(&g_conns_mu);
return 0;
}