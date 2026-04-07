#ifndef LOGOS_UUID_H
#define LOGOS_UUID_H

#include <random>
#include <string>

inline std::string logos_random_uuid_string()
{
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<int> dis(0, 15);
    const char* hex = "0123456789abcdef";
    auto nib = [&]() { return hex[dis(gen)]; };
    std::string s;
    s.reserve(36);
    for (int i = 0; i < 8; ++i)
        s += nib();
    s += '-';
    for (int i = 0; i < 4; ++i)
        s += nib();
    s += '-';
    s += '4';
    for (int i = 0; i < 3; ++i)
        s += nib();
    s += '-';
    s += hex[(dis(gen) & 0x3) | 0x8];
    for (int i = 0; i < 3; ++i)
        s += nib();
    s += '-';
    for (int i = 0; i < 12; ++i)
        s += nib();
    return s;
}

#endif
