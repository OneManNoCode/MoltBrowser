// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// A stand-in for tesseract and whisper-cli in the Windows helper sandbox
// tests (HelperSandboxWinTest in helper_sandbox_unittest.cc). It does one
// thing per command line and prints what happened, so the test can tell
// what the sandbox let it do:
//
//   echo TEXT        TEXT on stdout and on stderr; exit code 3
//   read PATH        "READ <contents>" or "DENIED <error>"
//   write PATH       creates PATH: "WROTE" or "DENIED <error>"
//   connect PORT     TCP to 127.0.0.1:PORT: "CONNECTED" or "REFUSED <error>"
//   spawn PROGRAM    starts PROGRAM: "SPAWNED" or "DENIED <error>"
//   alloc MIB        commits MIB MiB: "ALLOCATED" or "FAILED <error>"
//   env              its environment, one variable a line
//   spin             loops until killed
//
// Plain Win32 and Winsock, no //base, no user32: it loads only what the
// helpers themselves need from Windows.

#ifdef UNSAFE_BUFFERS_BUILD
// A C entry point's argv and the environment block are raw arrays, and
// this test-only program has no //base for spans.
#pragma allow_unsafe_buffers
#endif

// winsock2.h before windows.h, which would otherwise bring in winsock.h.
#include <winsock2.h>

#include <windows.h>

#include <fcntl.h>
#include <io.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

namespace {

void Print(const char* text) {
  fputs(text, stdout);
  fflush(stdout);
}

void PrintError(const char* what) {
  printf("%s %lu\n", what, GetLastError());
  fflush(stdout);
}

int Read(const wchar_t* path) {
  HANDLE file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, nullptr,
                            OPEN_EXISTING, 0, nullptr);
  if (file == INVALID_HANDLE_VALUE) {
    PrintError("DENIED");
    return 0;
  }
  char buffer[256] = {};
  DWORD bytes = 0;
  ReadFile(file, buffer, sizeof(buffer) - 1, &bytes, nullptr);
  CloseHandle(file);
  printf("READ %s\n", buffer);
  fflush(stdout);
  return 0;
}

int Write(const wchar_t* path) {
  HANDLE file =
      CreateFileW(path, GENERIC_WRITE, 0, nullptr, CREATE_NEW, 0, nullptr);
  if (file == INVALID_HANDLE_VALUE) {
    PrintError("DENIED");
    return 0;
  }
  DWORD bytes = 0;
  WriteFile(file, "written\n", 8, &bytes, nullptr);
  CloseHandle(file);
  Print("WROTE\n");
  return 0;
}

int Connect(const wchar_t* port) {
  WSADATA data;
  if (WSAStartup(MAKEWORD(2, 2), &data) != 0) {
    Print("REFUSED startup\n");
    return 0;
  }
  SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (s == INVALID_SOCKET) {
    printf("REFUSED socket %d\n", WSAGetLastError());
    fflush(stdout);
    return 0;
  }
  sockaddr_in address = {};
  address.sin_family = AF_INET;
  address.sin_port = htons(static_cast<u_short>(wcstoul(port, nullptr, 10)));
  address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  if (connect(s, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0) {
    Print("CONNECTED\n");
  } else {
    printf("REFUSED %d\n", WSAGetLastError());
    fflush(stdout);
  }
  closesocket(s);
  WSACleanup();
  return 0;
}

int Spawn(const wchar_t* program) {
  STARTUPINFOW startup = {};
  startup.cb = sizeof(startup);
  PROCESS_INFORMATION info = {};
  wchar_t command_line[] = L"child";
  if (!CreateProcessW(program, command_line, nullptr, nullptr, FALSE,
                      DETACHED_PROCESS, nullptr, nullptr, &startup, &info)) {
    PrintError("DENIED");
    return 0;
  }
  TerminateProcess(info.hProcess, 0);
  CloseHandle(info.hThread);
  CloseHandle(info.hProcess);
  Print("SPAWNED\n");
  return 0;
}

int Alloc(const wchar_t* mib) {
  const SIZE_T bytes =
      static_cast<SIZE_T>(wcstoull(mib, nullptr, 10)) * 1024 * 1024;
  void* memory =
      VirtualAlloc(nullptr, bytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
  if (!memory) {
    PrintError("FAILED");
    return 0;
  }
  VirtualFree(memory, 0, MEM_RELEASE);
  Print("ALLOCATED\n");
  return 0;
}

int Env() {
  wchar_t* block = GetEnvironmentStringsW();
  for (const wchar_t* entry = block; entry && *entry;
       entry += wcslen(entry) + 1) {
    printf("%ls\n", entry);
  }
  fflush(stdout);
  FreeEnvironmentStringsW(block);
  return 0;
}

}  // namespace

int wmain(int argc, wchar_t* argv[]) {
  // "\n" as written, not "\r\n": the test compares bytes.
  _setmode(_fileno(stdout), _O_BINARY);
  _setmode(_fileno(stderr), _O_BINARY);
  if (argc == 3 && wcscmp(argv[1], L"echo") == 0) {
    printf("%ls\n", argv[2]);
    fflush(stdout);
    fprintf(stderr, "%ls\n", argv[2]);
    fflush(stderr);
    return 3;
  }
  if (argc == 3 && wcscmp(argv[1], L"read") == 0) {
    return Read(argv[2]);
  }
  if (argc == 3 && wcscmp(argv[1], L"write") == 0) {
    return Write(argv[2]);
  }
  if (argc == 3 && wcscmp(argv[1], L"connect") == 0) {
    return Connect(argv[2]);
  }
  if (argc == 3 && wcscmp(argv[1], L"spawn") == 0) {
    return Spawn(argv[2]);
  }
  if (argc == 3 && wcscmp(argv[1], L"alloc") == 0) {
    return Alloc(argv[2]);
  }
  if (argc == 2 && wcscmp(argv[1], L"env") == 0) {
    return Env();
  }
  if (argc == 2 && wcscmp(argv[1], L"spin") == 0) {
    for (volatile unsigned counter = 0;; counter = counter + 1) {
    }
  }
  Print("usage: echo|read|write|connect|spawn|alloc|env|spin ...\n");
  return 2;
}
