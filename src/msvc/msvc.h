#ifndef MSVC_H
#define MSVC_H

#define MSVC_MAX(a, b) (((a) > (b)) ? (a) : (b))

char* strset(char* str, int c);
char* strupr(char* str); // Declaration for strupr
int stricmp(const char* str1, const char* str2); // Declaration for stricmp

#endif //MSVC_H