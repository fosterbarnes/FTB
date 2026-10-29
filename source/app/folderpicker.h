#pragma once

// SD folder browser that returns one folder. onPicked runs after the picker has closed, so it may
// open the next view; it does not run when the user backs out.
void folderpicker_open(const char* title, void* data, void (*onPicked)(void* data, const char* path));
