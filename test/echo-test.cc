#include <unistd.h>
#include <string>

#include "test.h"

int main(void)
{
  struct connection c1;
  struct connection c2;

  // Bigger buffers so long/pipelined tests don't get truncated.
  initializeBuffers(&c1, 30000);
  initializeBuffers(&c2, 30000);

  // ------------------------------------------------------------
  // 1) Connect + greeting (no extra data after greeting)
  // ------------------------------------------------------------
  connectToPort(&c1, 10000);
  expectToRead(&c1, "+(Author: *");
  expectNoMoreData(&c1);

  // ------------------------------------------------------------
  // 2) ECHO with NO argument (just "ECHO\r\n")
  //    Expected: "+OK" (no extra space, no text)
  // ------------------------------------------------------------
  writeString(&c1, "ECHO\r\n");
  expectToRead(&c1, "+OK");
  expectNoMoreData(&c1);

  // ------------------------------------------------------------
  // 3) Unknown command with args still unknown
  // ------------------------------------------------------------
  writeString(&c1, "BLAH something here\r\n");
  expectToRead(&c1, "-ERR Unknown command");
  expectNoMoreData(&c1);

  // ------------------------------------------------------------
  // 4) QUIT in a pipeline: anything after QUIT should NOT run
  // ------------------------------------------------------------
  writeString(&c1, "ECHO first\r\nQUIT\r\nECHO should_not_happen\r\n");
  expectToRead(&c1, "+OK first");
  expectToRead(&c1, "+OK Goodbye!");
  expectRemoteClose(&c1);
  closeConnection(&c1);

  // reopen for more
  connectToPort(&c1, 10000);
  expectToRead(&c1, "+OK Server ready (Author: *");
  expectNoMoreData(&c1);

  // ------------------------------------------------------------
  // 5) Very “TCP-ish” splitting:
  //    - split inside the command word
  //    - then split CRLF across writes
  // ------------------------------------------------------------
  writeString(&c1, "EC");
  expectNoMoreData(&c1);
  writeString(&c1, "HO hi");
  expectNoMoreData(&c1);
  writeString(&c1, "\r");
  expectNoMoreData(&c1);
  writeString(&c1, "\n");
  expectToRead(&c1, "+OK hi");
  expectNoMoreData(&c1);

  // ------------------------------------------------------------
  // 6) Multiple commands in ONE write (stress parsing loop)
  // ------------------------------------------------------------
  {
    std::string big;
    for (int i = 1; i <= 25; i++) {
      big += "ECHO msg" + std::to_string(i) + "\r\n";
    }
    writeString(&c1, big.c_str());

    for (int i = 1; i <= 25; i++) {
      expectToRead(&c1, ("+OK msg" + std::to_string(i)).c_str());
    }
    expectNoMoreData(&c1);
  }

  // ------------------------------------------------------------
  // 7) Client disconnects without QUIT:
  //    server must NOT crash; we should be able to reconnect.
  // ------------------------------------------------------------
  closeConnection(&c1);

  connectToPort(&c1, 10000);
  expectToRead(&c1, "+OK Server ready (Author: *");
  expectNoMoreData(&c1);

  // ------------------------------------------------------------
  // 8) Two clients: one “hangs” with partial command,
  //    the other must still work immediately (forces threading).
  // ------------------------------------------------------------
  connectToPort(&c2, 10000);
  expectToRead(&c2, "+OK Server ready (Author: *");
  expectNoMoreData(&c2);

  writeString(&c1, "ECHO partial_no_newline_yet");
  expectNoMoreData(&c1);               // no full line yet => no response

  writeString(&c2, "ECHO other_client_ok\r\n");
  expectToRead(&c2, "+OK other_client_ok");
  expectNoMoreData(&c2);

  // finish c1
  writeString(&c1, "\r\n");
  expectToRead(&c1, "+OK partial_no_newline_yet");
  expectNoMoreData(&c1);

  // ------------------------------------------------------------
  // 9) QUIT both clients cleanly
  // ------------------------------------------------------------
  writeString(&c2, "QUIT\r\n");
  expectToRead(&c2, "+OK Goodbye!");
  expectRemoteClose(&c2);
  closeConnection(&c2);

  writeString(&c1, "QUIT\r\n");
  expectToRead(&c1, "+OK Goodbye!");
  expectRemoteClose(&c1);
  closeConnection(&c1);

  freeBuffers(&c1);
  freeBuffers(&c2);
  return 0;
}
