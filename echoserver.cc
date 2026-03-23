// CIS 5050 HW2 – Milestone 1: multithreaded echo server.
// Cleanup: on Ctrl+C send -ERR Server shutting down to each connection and close all sockets.

#include <cstdlib>
#include <cstdio>
#include <getopt.h>
#include <unistd.h>
#include <iostream>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <cerrno>
#include <cstring>
#include <cstdint>
#include <csignal>
#include <string>
#include <pthread.h>
#include <cctype>
#include <set>
#include <vector>

using namespace std;

static volatile sig_atomic_t g_shutdown = 0;
static int g_listen_fd = -1;
static pthread_mutex_t g_conn_mu = PTHREAD_MUTEX_INITIALIZER;
static set<int> g_open_conns;

// Add a connection file descriptor to the global tracking set.
static void track_add(int fd) {
  pthread_mutex_lock(&g_conn_mu);
  g_open_conns.insert(fd);
  pthread_mutex_unlock(&g_conn_mu);
}

// Remove a connection file descriptor from the global tracking set.
static void track_remove(int fd) {
  pthread_mutex_lock(&g_conn_mu);
  g_open_conns.erase(fd);
  pthread_mutex_unlock(&g_conn_mu);
}

// Return an upper‑cased copy of the input string (for case‑insensitive commands).
static string upper_copy(string s) {          // <-- add helper
  for (char &c : s) c = (char)toupper((unsigned char)c);
  return s;
}

// SIGINT handler: mark shutdown and close the listening socket to wake accept().
static void on_sigint(int) {
  g_shutdown = 1;                 // tell main loop to stop

  // This makes accept() wake up / fail so main can exit the loop.
  if (g_listen_fd >= 0) {
    close(g_listen_fd);
    g_listen_fd = -1;
  }
}

// Install SIGINT handler using sigaction (no SA_RESTART).
static void install_sigint_handler() {
  struct sigaction sa{};
  sa.sa_handler = on_sigint;
  sigemptyset(&sa.sa_mask);
  sa.sa_flags = 0;                // don't auto-restart accept()
  sigaction(SIGINT, &sa, nullptr);
}


// Create, bind, and listen on a TCP socket on the given port.
int make_listen_socket (int port) {
  int fd = socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0) {
    cerr << "Socket() failed: " << strerror(errno) << "\n";
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
  addr.sin_port = htons(port);
  if (bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
    cerr << "bind() failed: " << strerror(errno) << "\n";
    exit(1);
  }
  if (listen(fd, 100) < 0) {
    cerr << "listen() failed: " << strerror(errno) << "\n";
    exit(1);
  }
  return fd;
}

// Read a single \r\n‑terminated line from fd into out (without CR/LF).
// Returns true on success, false on EOF or error.
bool recv_line (int fd, string &out) {
  out.clear();
  char ch;
  while (true) {
    ssize_t n = recv (fd, &ch, 1, 0);
    if (n == 0) return false;
    if (n < 0) {
      if (errno == EINTR) continue;
      return false;
    }
    if (ch == '\n') break;
    if (ch != '\r') out.push_back(ch);
    if (out.size() > 8192) return false;
  }
  return true;
}

// Handle a single client connection: send greeting, process ECHO/QUIT/unknown commands.
void handle_client(int conn_fd, bool verbose) {
  string greeting = "+OK Server ready (Author: Anshu Gupta / anshuykg)\r\n";

  if (verbose) {
    cerr << "[" << conn_fd << "] S: " << greeting.substr(0, greeting.size()-2) << "\n";
  }

  ssize_t n = send(conn_fd, greeting.c_str(), greeting.size(), 0);
  if (n < 0) {
    if (verbose) cerr << "[" << conn_fd << "] Connection closed\n";
    track_remove(conn_fd);
    close(conn_fd);
    return;
  }
  while (true) {
    string line;
    bool ok = recv_line(conn_fd, line);
    if (!ok) {
      if (verbose) cerr << "[" << conn_fd << "] Connection closed\n";
      break;
    }
    if (verbose) cerr << "[" << conn_fd << "] C: " << line << "\n";

    string cmd, rest;
    size_t sp = line.find(' ');
    if (sp == string::npos) {
      cmd = line;
      rest = "";
    } else {
      cmd = line.substr(0, sp);
      rest = line.substr(sp+1);
    }
    cmd = upper_copy(cmd);

    string resp;
    if (cmd == "ECHO") {
      resp = "+OK " + rest + "\r\n";
    } else if (cmd == "QUIT") {
      resp = "+OK Goodbye!\r\n";
    } else {
      resp = "-ERR Unknown command\r\n";
    }

    ssize_t w = send(conn_fd, resp.c_str(), resp.size(), 0);
    if (w < 0) {
      if (verbose) cerr << "[" << conn_fd << "] Connection closed\n";
      break;
    }
    if (cmd == "QUIT") {
      if (verbose) cerr << "[" << conn_fd << "] Connection closed\n";
      break;
    }
  }
  track_remove(conn_fd);
  close(conn_fd);
}

// Arguments passed to each worker thread.
struct ThreadArgs {
  int conn_fd;
  bool verbose;
};

// Worker thread entry: serve one client, then exit.
void *client_thread(void *arg) {
  ThreadArgs *a = static_cast<ThreadArgs *>(arg);
  int fd = a->conn_fd;
  bool verbose = a->verbose;
  delete a;

  handle_client(fd, verbose);
  return nullptr;
}

// Send shutdown message to all currently tracked connections and close them.
static void shutdown_all_connections (bool verbose) {
  const string msg = "-ERR Server shutting down\r\n";
  vector<int> fds;
  pthread_mutex_lock(&g_conn_mu);
  for (int fd : g_open_conns) fds.push_back(fd);
  pthread_mutex_unlock(&g_conn_mu);

   // send message + close each connection
   for (int fd : fds) {
    if (verbose) cerr << "[" << fd << "] S: -ERR Server shutting down\n";
    send(fd, msg.c_str(), msg.size(), 0);
    close(fd);
    if (verbose) cerr << "[" << fd << "] Connection closed\n";
    track_remove(fd);
  }
}

// Parse options, start listening, accept connections and spawn worker threads.
// On Ctrl+C, cleanly shut down all active connections.
int main(int argc, char *argv[]) {
  signal(SIGPIPE, SIG_IGN);
  int opt, port = 10000;
  bool verbose = false;

  while ((opt = getopt(argc, argv, "p:va")) != -1) {
    switch (opt) {
      case 'p': port = atoi(optarg); break;
      case 'v': verbose = true; break;
      case 'a': cerr << "Full name: Anshu Gupta\nSEAS login: anshuykg\n"; return 0;
      default: cerr << "Usage: " << argv[0] << " [-p port] [-v] [-a]\n"; return 1;
    }
  }

  int listen_fd = make_listen_socket(port);

  g_listen_fd = listen_fd;
  install_sigint_handler();

  while (true) {
    sockaddr_in client{};
    socklen_t len = sizeof(client);
    int conn_fd = accept(listen_fd, (sockaddr *)&client, &len);

    if (conn_fd < 0) {
      if (g_shutdown) break;
      if (errno == EINTR) continue;
      cerr << "accept() failed: " << strerror(errno) << "\n";
      continue;
    }
    track_add (conn_fd);

    if (verbose) cerr << "[" << conn_fd << "] New connection\n";

    pthread_t tid;
    ThreadArgs *args = new ThreadArgs{conn_fd, verbose};
    int rc = pthread_create(&tid, nullptr, client_thread, args);
    if (rc != 0) {
      cerr << "pthread_create failed: " << strerror(rc) << "\n";
      track_remove(conn_fd);
      close(conn_fd);
      delete args;
      continue;
    }
    pthread_detach(tid);
  }
  shutdown_all_connections(verbose);

  return 0;
}
