#ifndef HELPERS_H
#define HELPERS_H

#include <string>
#include <vector>
using namespace std;

// Given an ordering, return a list of swaps to put a list in that order.
// This ordering can be applied with run_swaps.
void make_swaps(vector<int> order, vector<pair<int,int>> &swaps);

template<typename T>
void run_swaps(T data, const vector<pair<int,int>> &swaps)
{
    for(const auto &s: swaps)
    {
	int src = s.first;
	int dst = s.second;
	swap(data[src], data[dst]);
    }
}

string vssprintf(const char *fmt, va_list va);
string ssprintf(const char *fmt, ...);
string subst(string s, string from, string to);
string basename(const string &dir);
string setExtension(string path, const string &ext);

#endif
