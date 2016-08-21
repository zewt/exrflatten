#include <stdarg.h>

#include <string>
using namespace std;

string vssprintf(const char *fmt, va_list va)
{
    // Work around a gcc bug: passing a va_list to vsnprintf alters it. va_list is supposed
    // to be by value.
    va_list vc;
    va_copy(vc, va);

    int iBytes = vsnprintf(NULL, 0, fmt, vc);
    char *pBuf = (char*) alloca(iBytes + 1);
    vsnprintf(pBuf, iBytes + 1, fmt, va);
    return string(pBuf, iBytes);
}

string ssprintf(const char *fmt, ...)
{
    va_list va;
    va_start(va, fmt);
    return vssprintf(fmt, va);
}

string subst(string s, string from, string to)
{
    int start = 0;
    while(1)
    {
        auto pos = s.find(from, start);
        if(pos == string::npos)
            break;

        string before = s.substr(0, pos);
        string after = s.substr(pos + from.size());
        s = before + to + after;
        start = pos + to.size();
    }

    return s;
}

/*
 * Return the last named component of dir:
 * a/b/c -> c
 * a/b/c/ -> c
 */
string basename(const string &dir)
{
    size_t end = dir.find_last_not_of("/\\");
    if( end == dir.npos )
        return "";

    size_t start = dir.find_last_of("/\\", end);
    if(start == dir.npos)
        start = 0;
    else
        ++start;

    return dir.substr(start, end-start+1);
}

string setExtension(string path, const string &ext)
{
    auto pos = path.rfind('.');
    if(pos != string::npos)
        path = path.substr(0, pos);

    return path + ext;
}
