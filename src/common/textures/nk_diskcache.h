#pragma once

class FBitmap;

void NKDiskCache_Init();
bool NKDiskCache_TryLoadBitmap(int sourceLump, int width, int height, int frame, FBitmap &bitmap, int *trans);
void NKDiskCache_SaveBitmap(int sourceLump, int width, int height, int frame, const FBitmap &bitmap, int trans);
void NKDiskCache_Shutdown();
