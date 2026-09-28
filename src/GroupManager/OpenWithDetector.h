#pragma once
#include <Windows.h>
#include <string>

bool OpenWithDetector_Start(HWND hIpcWnd);
void OpenWithDetector_Stop();

std::wstring DndGetAccessibleName(POINT ptScreen);
std::wstring DndFindLnkByDisplayName(const std::wstring& displayName);
