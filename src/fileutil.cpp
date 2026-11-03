#include "fileutil.h"
#ifdef _WIN32
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <commdlg.h>
#include <shlobj.h>

bool fileDialogSupported() { return true; }

bool pickOpenFile(const char* filter, std::string& out) {
  char buf[1024] = {0};
  OPENFILENAMEA ofn;
  ZeroMemory(&ofn, sizeof(ofn));
  ofn.lStructSize = sizeof(ofn);
  ofn.lpstrFilter = filter;
  ofn.lpstrFile = buf;
  ofn.nMaxFile = sizeof(buf);
  ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
  if (GetOpenFileNameA(&ofn)) { out = buf; return true; }
  return false;
}

bool pickFolder(std::string& out) {
  CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
  char buf[MAX_PATH] = {0};
  BROWSEINFOA bi;
  ZeroMemory(&bi, sizeof(bi));
  bi.lpszTitle = "Pilih folder export";
  bi.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;
  LPITEMIDLIST pidl = SHBrowseForFolderA(&bi);
  if (!pidl) return false;
  bool ok = SHGetPathFromIDListA(pidl, buf) != 0;
  CoTaskMemFree(pidl);
  if (ok) out = buf;
  return ok;
}
#else
bool fileDialogSupported() { return false; }
bool pickOpenFile(const char*, std::string&) { return false; }
bool pickFolder(std::string&) { return false; }
#endif
