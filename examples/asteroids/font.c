#include "font.h"

#include <ctype.h>
#include <string.h>

/*
 * Segments between points of a 3x3 grid (in quarter units, 0..4, y down),
 * named a..p, plus q..u for the rounded D:
 *   a b   top halves          i j   middle halves
 *   c d   right upper, lower  k l m diagonals and vertical, upper half
 *   e f   bottom halves       n o p diagonals and vertical, lower half
 *   g h   left lower, upper   q..u  D's top, corners, right side and bottom
 */
static const signed char SEG[21][4] = {
    {0, 0, 2, 0}, {2, 0, 4, 0},  /* a b */
    {4, 0, 4, 2}, {4, 2, 4, 4},  /* c d */
    {4, 4, 2, 4}, {2, 4, 0, 4},  /* e f */
    {0, 4, 0, 2}, {0, 2, 0, 0},  /* g h */
    {0, 2, 2, 2}, {2, 2, 4, 2},  /* i j */
    {0, 0, 2, 2}, {2, 0, 2, 2}, {4, 0, 2, 2},  /* k l m */
    {2, 2, 4, 4}, {2, 2, 2, 4}, {2, 2, 0, 4},  /* n o p */
    {0, 0, 3, 0}, {3, 0, 4, 1}, {4, 1, 4, 3}, {4, 3, 3, 4}, {3, 4, 0, 4},  /* q r s t u */
};

static const char *glyph(char ch)
{
    static const char *digits[10] = {
        "abcdefghmp", "lo", "abcijgef", "abcdefj", "hijcd",
        "abhijdef", "abhgefdij", "abcd", "abcdefghij", "abhcijdef",
    };
    static const char *letters[26] = {
        "abcdghij",  /* A */ "ghqrstuij", /* B */ "abghef",    /* C */
        "ghqrstu",   /* D */ "abghefi",   /* E */ "abghi",     /* F */
        "abghefdj",  /* G */ "ghcdij",    /* H */ "abloef",    /* I */
        "cdefg",     /* J */ "ghimn",     /* K */ "ghef",      /* L */
        "ghcdkm",    /* M */ "ghcdkn",    /* N */ "abcdefgh",  /* O */
        "abcghij",   /* P */ "abcdefghn", /* Q */ "abcghijn",  /* R */
        "abhijdef",  /* S */ "ablo",      /* T */ "cdefgh",    /* U */
        "ghpm",      /* V */ "ghcdpn",    /* W */ "kmnp",      /* X */
        "kmo",       /* Y */ "abmpef",    /* Z */
    };
    if (ch >= '0' && ch <= '9')
        return digits[ch - '0'];
    ch = (char)toupper((unsigned char)ch);
    if (ch >= 'A' && ch <= 'Z')
        return letters[ch - 'A'];
    switch (ch) {
    case '-': return "ij";
    case '+': return "ijlo";
    case '=': return "ijef";
    case '/': return "mp";
    case '(': case '<': return "mn";
    case ')': case '>': return "kp";
    case '_': return "ef";
    case '\'': return "l";
    case '?': return "abcjo";
    case ',': return "p";
    case '#': return "ijlocd";
    default: return "";
    }
}

static void dot(SDL_Renderer *r, float x, float y, float s)
{
    SDL_FRect d = { x - s, y - s, 2 * s, 2 * s };
    SDL_RenderFillRectF(r, &d);
}

void font_draw(SDL_Renderer *r, float x, float y, float scale, const char *text)
{
    const float w = 1.5f * scale, h = 2.5f * scale;  /* quarter cell: glyph 6x10 + spacing */

    for (; *text; text++, x += 8.0f * scale) {
        const char *g = glyph(*text);
        if (*text == '.' || *text == ':' || *text == '!') {
            dot(r, x + 2 * w, y + 4 * h, scale * 0.8f);
            if (*text == ':')
                dot(r, x + 2 * w, y + h * 1.6f, scale * 0.8f);
            if (*text == '!')
                SDL_RenderDrawLineF(r, x + 2 * w, y, x + 2 * w, y + h * 2.8f);
            continue;
        }
        for (; *g; g++) {
            const signed char *s = SEG[*g - 'a'];
            SDL_RenderDrawLineF(r, x + s[0] * w, y + s[1] * h, x + s[2] * w, y + s[3] * h);
        }
    }
}

float font_width(float scale, const char *text)
{
    size_t n = strlen(text);
    return n ? (float)n * 8.0f * scale - 2.0f * scale : 0.0f;
}

void font_draw_centered(SDL_Renderer *r, float cx, float y, float scale, const char *text)
{
    font_draw(r, cx - font_width(scale, text) / 2.0f, y, scale, text);
}
