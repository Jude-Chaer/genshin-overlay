#include "lua_patterns.hpp"
#include <cctype>
#include <cstring>

// The walk follows the matcher in Lua 5.4's lstrlib.c step for step, so it
// takes the same turns Lua's own search will take, with a step count added.
// It keeps no results, only whether the search ends.

namespace LuaPatterns {
    namespace {
        struct Malformed {};
        struct TooLong {};

        // Lua's MAXCCALLS
        constexpr int MaxDepth = 200;
        // Lua's LUA_MAXCAPTURES
        constexpr int MaxCaptures = 32;
        constexpr char Esc = '%';
        constexpr ptrdiff_t Unfinished = -1;

        void (*spentSteps)(lua_State* L, long long steps) = nullptr;

        int uchar(char c) {
            return (unsigned char)c;
        }

        bool matchClass(int c, int cl) {
            bool res;
            switch (std::tolower(cl)) {
            case 'a': res = std::isalpha(c); break;
            case 'c': res = std::iscntrl(c); break;
            case 'd': res = std::isdigit(c); break;
            case 'g': res = std::isgraph(c); break;
            case 'l': res = std::islower(c); break;
            case 'p': res = std::ispunct(c); break;
            case 's': res = std::isspace(c); break;
            case 'u': res = std::isupper(c); break;
            case 'w': res = std::isalnum(c); break;
            case 'x': res = std::isxdigit(c); break;
            case 'z': res = c == 0; break;
            default: return cl == c;
            }
            return std::islower(cl) ? res : !res;
        }

        // p is the '[', ec the ']'
        bool matchBracket(int c, const char* p, const char* ec) {
            bool inside = true;
            if (*(p + 1) == '^') {
                inside = false;
                p++;
            }
            while (++p < ec) {
                if (*p == Esc) {
                    p++;
                    if (matchClass(c, uchar(*p))) return inside;
                }
                else if (*(p + 1) == '-' && p + 2 < ec) {
                    p += 2;
                    if (uchar(*(p - 2)) <= c && c <= uchar(*p)) return inside;
                }
                else if (uchar(*p) == c) return inside;
            }
            return !inside;
        }

        struct Walk {
            const char* begin;
            const char* end;
            const char* patternEnd;
            int depth = MaxDepth;
            int level = 0;
            struct {
                const char* init;
                ptrdiff_t len;
            } captures[MaxCaptures];
            long long steps = 0;

            void step(long long count = 1) {
                steps += count;
                if (steps > MaxSteps) throw TooLong{};
            }

            const char* classEnd(const char* p) {
                switch (*p++) {
                case Esc:
                    if (p == patternEnd) throw Malformed{};
                    return p + 1;
                case '[':
                    if (*p == '^') p++;
                    do {
                        if (p == patternEnd) throw Malformed{};
                        if (*(p++) == Esc && p < patternEnd) p++;
                    } while (*p != ']');
                    return p + 1;
                default:
                    return p;
                }
            }

            bool single(const char* s, const char* p, const char* ep) {
                step();
                if (s >= end) return false;
                int c = uchar(*s);
                switch (*p) {
                case '.': return true;
                case Esc: return matchClass(c, uchar(*(p + 1)));
                case '[': return matchBracket(c, p, ep - 1);
                default: return uchar(*p) == c;
                }
            }

            const char* balance(const char* s, const char* p) {
                if (p >= patternEnd - 1) throw Malformed{};
                if (*s != *p) return nullptr;
                char open = *p, close = *(p + 1);
                int depthOpen = 1;
                while (++s < end) {
                    step();
                    if (*s == close) {
                        if (--depthOpen == 0) return s + 1;
                    }
                    else if (*s == open) depthOpen++;
                }
                return nullptr;
            }

            const char* longest(const char* s, const char* p, const char* ep) {
                ptrdiff_t count = 0;
                while (single(s + count, p, ep)) count++;
                for (; count >= 0; count--) {
                    if (const char* res = match(s + count, ep + 1)) return res;
                }
                return nullptr;
            }

            const char* shortest(const char* s, const char* p, const char* ep) {
                while (true) {
                    if (const char* res = match(s, ep + 1)) return res;
                    if (!single(s, p, ep)) return nullptr;
                    s++;
                }
            }

            const char* openCapture(const char* s, const char* p, ptrdiff_t what) {
                if (level >= MaxCaptures) throw Malformed{};
                captures[level].init = s;
                captures[level].len = what;
                level++;
                const char* res = match(s, p);
                if (!res) level--;
                return res;
            }

            const char* closeCapture(const char* s, const char* p) {
                int open = level - 1;
                while (open >= 0 && captures[open].len != Unfinished) open--;
                if (open < 0) throw Malformed{};
                captures[open].len = s - captures[open].init;
                const char* res = match(s, p);
                if (!res) captures[open].len = Unfinished;
                return res;
            }

            const char* sameAsCapture(const char* s, int digit) {
                int index = digit - '1';
                if (index < 0 || index >= level || captures[index].len == Unfinished) throw Malformed{};
                size_t len = (size_t)captures[index].len;
                step(1 + (long long)(len / 64));
                if ((size_t)(end - s) >= len && std::memcmp(captures[index].init, s, len) == 0) return s + len;
                return nullptr;
            }

            const char* match(const char* s, const char* p) {
                if (depth-- == 0) throw Malformed{};
                step();
                while (p != patternEnd) {
                    if (*p == '(') {
                        s = *(p + 1) == ')' ? openCapture(s, p + 2, -2) : openCapture(s, p + 1, Unfinished);
                        break;
                    }
                    if (*p == ')') {
                        s = closeCapture(s, p + 1);
                        break;
                    }
                    if (*p == '$' && p + 1 == patternEnd) {
                        s = s == end ? s : nullptr;
                        break;
                    }
                    if (*p == Esc && *(p + 1) == 'b') {
                        s = balance(s, p + 2);
                        if (!s) break;
                        p += 4;
                        continue;
                    }
                    if (*p == Esc && *(p + 1) == 'f') {
                        p += 2;
                        if (*p != '[') throw Malformed{};
                        const char* ep = classEnd(p);
                        int previous = s == begin ? 0 : uchar(*(s - 1));
                        if (!matchBracket(previous, p, ep - 1) && matchBracket(uchar(*s), p, ep - 1)) {
                            p = ep;
                            continue;
                        }
                        s = nullptr;
                        break;
                    }
                    if (*p == Esc && std::isdigit(uchar(*(p + 1)))) {
                        s = sameAsCapture(s, uchar(*(p + 1)));
                        if (!s) break;
                        p += 2;
                        continue;
                    }

                    // one item, then *, +, -, ? or nothing
                    const char* ep = classEnd(p);
                    if (!single(s, p, ep)) {
                        if (*ep == '*' || *ep == '?' || *ep == '-') {
                            p = ep + 1;
                            continue;
                        }
                        s = nullptr;
                        break;
                    }
                    if (*ep == '?') {
                        if (const char* res = match(s + 1, ep + 1)) {
                            s = res;
                            break;
                        }
                        p = ep + 1;
                        continue;
                    }
                    if (*ep == '+') {
                        s = longest(s + 1, p, ep);
                        break;
                    }
                    if (*ep == '*') {
                        s = longest(s, p, ep);
                        break;
                    }
                    if (*ep == '-') {
                        s = shortest(s, p, ep);
                        break;
                    }
                    s++;
                    p = ep;
                }
                depth++;
                return s;
            }
        };

        void walk(Walk& w, const char* subject, size_t length, const char* pattern, size_t patternLength, Mode mode, size_t init,
            long long maxReplacements) {
            bool anchor = mode != Mode::Gmatch && patternLength > 0 && *pattern == '^';
            if (anchor) {
                pattern++;
                patternLength--;
            }
            w.begin = subject;
            w.end = subject + length;
            w.patternEnd = pattern + patternLength;
            const char* src = subject + init;

            if (mode == Mode::Find) {
                do {
                    w.level = 0;
                    if (w.match(src, pattern)) return;
                } while (src++ < w.end && !anchor);
                return;
            }

            const char* lastMatch = nullptr;
            if (mode == Mode::Gmatch) {
                while (true) {
                    bool found = false;
                    for (; src <= w.end; src++) {
                        w.level = 0;
                        const char* e = w.match(src, pattern);
                        if (e && e != lastMatch) {
                            src = lastMatch = e;
                            found = true;
                            break;
                        }
                    }
                    if (!found) return;
                }
            }

            long long replaced = 0;
            long long most = maxReplacements < 0 ? (long long)length + 1 : maxReplacements;
            while (replaced < most) {
                w.level = 0;
                const char* e = w.match(src, pattern);
                if (e && e != lastMatch) {
                    replaced++;
                    src = lastMatch = e;
                }
                else if (src < w.end) src++;
                else break;
                if (anchor) break;
            }
        }

        bool fitsRaw(const char* subject, size_t length, const char* pattern, size_t patternLength, Mode mode, size_t init,
            long long maxReplacements, long long* steps = nullptr) {
            Walk w;
            bool ok = true;
            try {
                walk(w, subject, length, pattern, patternLength, mode, init, maxReplacements);
            }
            catch (const TooLong&) {
                ok = false;
            }
            catch (const Malformed&) {
            }
            if (steps) *steps = w.steps;
            return ok;
        }

        // Lua's posrelatI, 1 based
        size_t startAt(lua_Integer pos, size_t length) {
            if (pos > 0) return (size_t)pos;
            if (pos == 0 || pos < -(lua_Integer)length) return 1;
            return length + (size_t)pos + 1;
        }

        bool noSpecials(const char* pattern, size_t length) {
            size_t upto = 0;
            do {
                if (std::strpbrk(pattern + upto, "^$*+?.([%-")) return false;
                upto += std::strlen(pattern + upto) + 1;
            } while (upto <= length);
            return true;
        }

        // the original function is upvalue 1
        int callOriginal(lua_State* L) {
            int given = lua_gettop(L);
            lua_pushvalue(L, lua_upvalueindex(1));
            lua_insert(L, 1);
            lua_call(L, given, LUA_MULTRET);
            return lua_gettop(L);
        }

        int guarded(lua_State* L, const char* name, Mode mode, bool find) {
            size_t length = 0, patternLength = 0;
            const char* subject = luaL_checklstring(L, 1, &length);
            const char* pattern = luaL_checklstring(L, 2, &patternLength);
            bool ok = true;
            long long steps = 0;
            if (mode == Mode::Gsub) {
                lua_Integer most = luaL_optinteger(L, 4, (lua_Integer)length + 1);
                ok = fitsRaw(subject, length, pattern, patternLength, mode, 0, most, &steps);
            }
            else {
                size_t init = startAt(luaL_optinteger(L, 3, 1), length) - 1;
                if (mode == Mode::Gmatch) {
                    if (init > length) init = length + 1;
                    ok = fitsRaw(subject, length, pattern, patternLength, mode, init, -1, &steps);
                }
                else if (init <= length && !(find && (lua_toboolean(L, 4) || noSpecials(pattern, patternLength)))) {
                    ok = fitsRaw(subject, length, pattern, patternLength, mode, init, -1, &steps);
                }
            }
            if (spentSteps && steps > 0) spentSteps(L, steps);
            if (!ok) return luaL_error(L, "%s: the pattern takes too long on this text", name);
            return callOriginal(L);
        }

        int find(lua_State* L) { return guarded(L, "string.find", Mode::Find, true); }
        int match(lua_State* L) { return guarded(L, "string.match", Mode::Find, false); }
        int gmatch(lua_State* L) { return guarded(L, "string.gmatch", Mode::Gmatch, false); }
        int gsub(lua_State* L) { return guarded(L, "string.gsub", Mode::Gsub, false); }
    }

    bool fits(const std::string& subject, const std::string& pattern, Mode mode, size_t init, long long maxReplacements) {
        return fitsRaw(subject.data(), subject.size(), pattern.data(), pattern.size(), mode, init, maxReplacements);
    }

    void guard(lua_State* L, void (*spent)(lua_State* L, long long steps)) {
        spentSteps = spent;
        const std::pair<const char*, lua_CFunction> functions[] = {
            { "find", find }, { "match", match }, { "gmatch", gmatch }, { "gsub", gsub },
        };
        lua_getglobal(L, "string");
        if (!lua_istable(L, -1)) {
            lua_pop(L, 1);
            return;
        }
        for (const auto& [name, function] : functions) {
            lua_getfield(L, -1, name);
            lua_pushcclosure(L, function, 1);
            lua_setfield(L, -2, name);
        }
        lua_pop(L, 1);
    }
}
