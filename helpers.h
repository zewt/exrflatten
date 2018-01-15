#ifndef HELPERS_H
#define HELPERS_H

#include <string>
#include <vector>
#include <map>
#include <unordered_map>
using namespace std;

template<typename K, typename V, typename def>
V map_get(const map<K,V> &m, K key, def defaultValue)
{
    auto iter = m.find(key);
    if(iter != m.end())
	return iter->second;
    return defaultValue;
}

template<typename K, typename V, typename def>
V map_get(const unordered_map<K,V> &m, K key, def defaultValue)
{
    auto iter = m.find(key);
    if(iter != m.end())
	return iter->second;
    return defaultValue;
}

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
void split(const string &source, const string &delimitor, vector<string> &result, const bool ignoreEmpty=true);

template<typename T>
T clamp(T value, T low, T high)
{
    // If low and high are flipped, correct it.  This happens for eg.
    // scale_clamp(value, 0, 1, 1, 0).
    T low1 = min(low, high);
    T high1 = max(low, high);
    return min(max(value, low1), high1);
}

template<typename T>
T scale(T value, T l1, T h1, T l2, T h2)
{
    return (value - l1) * (h2 - l2) / (h1 - l1) + l2;
}

template<typename T>
T scale_clamp(T value, T l1, T h1, T l2, T h2)
{
    return ::clamp(scale(value, l1, h1, l2, h2), l2, h2);
}

class StringException: public exception
{
public:
    StringException(string s)
    {
	value = s;
    }

    const char *what() const { return value.c_str(); }
private:
    string value;
};

#endif
