#ifndef HELPERS_H
#define HELPERS_H

#include <string>
using namespace std;

string vssprintf(const char *fmt, va_list va);
string ssprintf(const char *fmt, ...);
string subst(string s, string from, string to);
string basename(const string &dir);
string setExtension(string path, const string &ext);

#endif
