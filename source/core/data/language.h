#pragma once

// The console language (CFG_Language), read from CFG once; Japanese when CFG fails.
u8 language_system();
// The language to show a title in: the console language when the title's text in that language
// (UTF-16) is not blank, else the usual language of the console's region.
u8 language_pick(const u16* systemText);
