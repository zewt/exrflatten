#include "helpers.h"
#include <stdarg.h>

#include <string>
using namespace std;

void make_swaps(vector<int> order, vector<pair<int,int>> &swaps)
{
    // order[0] is the index in the old list of the new list's first value.
    // Invert the mapping: inverse[0] is the index in the new list of the
    // old list's first value.
    vector<int> inverse(order.size());
    for(int i = 0; i < order.size(); ++i)
        inverse[order[i]] = i;

    swaps.resize(0);

    for(int idx1 = 0; idx1 < order.size(); ++idx1)
    {
        // Swap list[idx] with list[order[idx]], and record this swap.
        int idx2 = order[idx1];
        if(idx1 == idx2)
            continue;

        swaps.push_back(make_pair(idx1, idx2));

        // list[idx1] is now in the correct place, but whoever wanted the value we moved out
        // of idx2 now needs to look in its new position.
        int idx1_dep = inverse[idx1];
        order[idx1_dep] = idx2;
        inverse[idx2] = idx1_dep;
    }
}

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

template <class S>
static int DelimitorLength( const S &Delimitor )
{
    return Delimitor.size();
}

static int DelimitorLength( char Delimitor )
{
    return 1;
}

static int DelimitorLength( wchar_t Delimitor )
{
    return 1;
}

template <class S, class C>
void do_split(const S &source, const C delimitor, vector<S> &result, const bool ignoreEmpty)
{
    /* Short-circuit if the source is empty; we want to return an empty vector if
     * the string is empty, even if bIgnoreEmpty is true. */
    if(source.empty())
        return;

    size_t startpos = 0;

    do {
        size_t pos;
        pos = source.find(delimitor, startpos);
        if(pos == source.npos)
            pos = source.size();

        if(pos-startpos > 0 || !ignoreEmpty)
        {
            /* Optimization: if we're copying the whole string, avoid substr; this
             * allows this copy to be refcounted, which is much faster. */
            if(startpos == 0 && pos-startpos == source.size())
                result.push_back(source);
            else
            {
                const S Addstring = source.substr(startpos, pos-startpos);
                result.push_back(Addstring);
            }
        }

        startpos = pos+DelimitorLength(delimitor);
    } while(startpos <= source.size());
}

void split(const string &source, const string &delimitor, vector<string> &result, const bool ignoreEmpty)
{
    if(delimitor.size() == 1)
        do_split(source, delimitor[0], result, ignoreEmpty);
    else
        do_split(source, delimitor, result, ignoreEmpty);
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

string getExtension(string path)
{
    auto pos = path.rfind('.');
    if(pos == string::npos)
        return "";
    return path.substr(pos+1);
}

string setExtension(string path, const string &ext)
{
    auto pos = path.rfind('.');
    if(pos != string::npos)
        path = path.substr(0, pos);

    return path + ext;
}

float LinearToSRGB(float value)
{
    static vector<float> table;
    if(table.empty())
    {
        vector<float> new_table;
        new_table.resize(65536);
        for(int i = 0; i < new_table.size(); i++)
        {
            float value = i / 65535.0f;
            float output;
            if(value <= 0.0031308f)
                output = value * 12.92f;
            else
                output = 1.055f*powf(value, 1/2.4f) - 0.055f;
            new_table[i] = output;
        }
        table = new_table;
    }

    if(value < 0) return 0;
    if(value > 1) return 1;
    int idx = int(value * 65535);
    return table[idx];
}

float SRGBToLinear(float value)
{
    static vector<float> table;
    if(table.empty())
    {
        vector<float> new_table;
        new_table.resize(65536);

        for(int i = 0; i < new_table.size(); i++)
        {
            float value = i / 65535.0f;
            float output;
            if(value <= 0.04045f)
                output = value / 12.92f;
            else
                output = powf((value + 0.055f) / 1.055f, 2.4f);
            new_table[i] = output;
        }

        table = new_table;
    }

    if(value < 0) return 0;
    if(value > 1) return 1;
    int idx = int(value * 65535);
    return table[idx];
}