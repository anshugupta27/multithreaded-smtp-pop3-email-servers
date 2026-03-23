#include <stdlib.h>
#include <stdio.h>
#include <openssl/md5.h>
#include <getopt.h>
#include <unistd.h>
#include <iostream>
#include <string>
#include <cerrno>
#include <cstring>
#include <csignal>
#include <pthread.h>

using namespace std;

void computeDigest(char *data, int dataLengthBytes, unsigned char *digestBuffer)
{
  /* The digest will be written to digestBuffer, which must be at least MD5_DIGEST_LENGTH bytes long */
  MD5_CTX c;
  MD5_Init(&c);
  MD5_Update(&c, data, dataLengthBytes);
  MD5_Final(digestBuffer, &c);
}

int main(int argc, char *argv[])
{
  signal(SIGPIPE, SIG_IGN);
  int port = 1100;
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
  string maildir = argv[optind];
  // struct stat st{};


  }