#pragma once
#include <string>
// Dialog file native (Windows). Di OS lain: tidak didukung -> pakai drag & drop.
bool fileDialogSupported();
bool pickOpenFile(const char* filter, std::string& out);   // filter: "Nama\0*.ext\0\0"
bool pickFolder(std::string& out);
