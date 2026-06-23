/**
 * @file text_emotion.c
 * @brief Real-time text emotion analyzer — keyword-driven expression control.
 * @version 1.0
 * @date 2025-03-29
 * @copyright Copyright (c) Tuya Inc.
 */
#include "text_emotion.h"

#include <string.h>
#include <stdio.h>
#include <time.h>

extern void vrm_viewer_set_emotion(const char *name, float intensity, float speed);

/* ---------------------------------------------------------------------------
 * Macros
 * --------------------------------------------------------------------------- */
#define SENTENCE_BUF_SIZE 512
#define MIN_SCAN_LEN      4

/* Minimum interval (ms) between two emotion switches to avoid flicker. */
#define EMOTION_COOLDOWN_MS 1500

/* How long (ms) a keyword-triggered emotion stays before reverting to base. */
#define KEYWORD_DECAY_MS    3000

#define BASE_EMO_MAX 32

/* ---------------------------------------------------------------------------
 * Type definitions
 * --------------------------------------------------------------------------- */
typedef struct {
    const char *keyword;
    int         kw_len;
    const char *emotion;
    int         priority;
} keyword_rule_t;

/* ---------------------------------------------------------------------------
 * File scope variables
 * --------------------------------------------------------------------------- */
static char s_buf[SENTENCE_BUF_SIZE];
static int  s_buf_len       = 0;
static unsigned int s_last_switch_ms = 0;
static const char  *s_current_emo    = "neutral";

static char         s_base_emo[BASE_EMO_MAX] = "neutral";
static unsigned int s_revert_at_ms           = 0;
static int          s_in_keyword_emo         = 0;

/* ---------------------------------------------------------------------------
 * Keyword table
 *
 * Rules are scanned in order; higher priority wins when multiple match.
 * Priority: 3 = strong signal, 2 = moderate, 1 = weak/ambient.
 * Covers Chinese and English keywords.
 * --------------------------------------------------------------------------- */
static keyword_rule_t s_rules[] = {
    /* ---- Happy / Joy ---- */
    {"哈哈",       0, "laughing",    3},
    {"笑死",       0, "laughing",    3},
    {"太好笑",     0, "laughing",    3},
    {"乐死",       0, "laughing",    3},
    {"哈哈哈",     0, "laughing",    3},
    {"开心",       0, "happy",       2},
    {"高兴",       0, "happy",       2},
    {"快乐",       0, "happy",       2},
    {"棒",         0, "happy",       1},
    {"好的",       0, "happy",       1},
    {"太好了",     0, "happy",       2},
    {"耶",         0, "happy",       2},
    {"好棒",       0, "happy",       2},
    {"恭喜",       0, "happy",       2},

    /* ---- Sad ---- */
    {"难过",       0, "sad",         2},
    {"伤心",       0, "sad",         2},
    {"悲伤",       0, "sad",         2},
    {"心疼",       0, "sad",         2},
    {"可惜",       0, "sad",         2},
    {"遗憾",       0, "sad",         2},
    {"哭",         0, "sad",         2},
    {"呜呜",       0, "sad",         3},
    {"呜",         0, "sad",         2},
    {"555",        0, "sad",         2},
    {"困难",       0, "sad",         2},

    /* ---- Angry ---- */
    {"生气",       0, "angry",       2},
    {"愤怒",       0, "angry",       3},
    {"讨厌",       0, "angry",       2},
    {"烦死了",     0, "angry",       2},
    {"气死",       0, "angry",       3},
    {"可恶",       0, "angry",       2},

    /* ---- Surprised / Shocked ---- */
    {"天哪",       0, "surprised",   2},
    {"哇",         0, "surprised",   2},
    {"啊",         0, "surprised",   1},
    {"什么",       0, "surprised",   1},
    {"真的吗",     0, "surprised",   2},
    {"不会吧",     0, "shocked",     2},
    {"我的天",     0, "shocked",     3},
    {"吓死",       0, "shocked",     3},
    {"震惊",       0, "shocked",     3},
    {"居然",       0, "surprised",   2},

    /* ---- Thinking ---- */
    {"让我想想",   0, "thinking",    3},
    {"思考",       0, "thinking",    2},
    {"嗯",         0, "thinking",    1},
    {"想一下",     0, "thinking",    2},
    {"想想",       0, "thinking",    2},
    {"考虑",       0, "thinking",    2},

    /* ---- Embarrassed ---- */
    {"害羞",       0, "embarrassed", 2},
    {"不好意思",   0, "embarrassed", 2},
    {"尴尬",       0, "embarrassed", 2},
    {"脸红",       0, "embarrassed", 2},

    /* ---- Loving ---- */
    {"喜欢",       0, "loving",      2},
    {"爱你",       0, "loving",      3},
    {"爱",         0, "loving",      1},
    {"亲亲",       0, "kissy",       3},
    {"么么",       0, "kissy",       3},
    {"心动",       0, "loving",      2},

    /* ---- Fearful ---- */
    {"害怕",       0, "fearful",     2},
    {"好怕",       0, "fearful",     2},
    {"恐怖",       0, "fearful",     2},
    {"可怕",       0, "fearful",     2},
    {"吓人",       0, "fearful",     2},

    /* ---- Cool / Confident ---- */
    {"酷",         0, "cool",        1},
    {"帅",         0, "cool",        1},
    {"厉害",       0, "confident",   2},
    {"没问题",     0, "confident",   2},
    {"交给我",     0, "confident",   2},
    {"包在我身上", 0, "confident",   3},

    /* ---- Confused ---- */
    {"奇怪",       0, "confused",    2},
    {"搞不懂",     0, "confused",    2},
    {"为什么",     0, "confused",    1},
    {"什么意思",   0, "confused",    2},
    {"困惑",       0, "confused",    2},

    /* ---- Relaxed / Sleep ---- */
    {"困了",         0, "sleep",       1},
    {"好困",       0, "sleep",       2},
    {"打哈欠",     0, "sleep",       2},
    {"放松",       0, "relaxed",     1},
    {"悠闲",       0, "relaxed",     2},

    /* ---- Silly / Playful ---- */
    {"嘻嘻",       0, "silly",       2},
    {"嘿嘿",       0, "silly",       2},
    {"傻",         0, "silly",       1},
    {"调皮",       0, "silly",       2},

    /* ---- Wink ---- */
    {"眨眼",       0, "wink",        2},

    /* ---- Delicious ---- */
    {"好吃",       0, "delicious",   2},
    {"美味",       0, "delicious",   2},
    {"好香",       0, "delicious",   2},
    {"馋",         0, "delicious",   2},

    /* ---- English keywords (lowercase match) ---- */
    {"haha",       0, "laughing",    3},
    {"lol",        0, "laughing",    2},
    {"happy",      0, "happy",       2},
    {"great",      0, "happy",       1},
    {"wonderful",  0, "happy",       2},
    {"sad",        0, "sad",         2},
    {"sorry",      0, "sad",         1},
    {"angry",      0, "angry",       2},
    {"wow",        0, "surprised",   2},
    {"oh no",      0, "shocked",     2},
    {"think",      0, "thinking",    1},
    {"hmm",        0, "thinking",    2},
    {"love",       0, "loving",      2},
    {"scared",     0, "fearful",     2},
    {"cool",       0, "cool",        1},
    {"confused",   0, "confused",    2},
    {"sleepy",     0, "sleep",       2},
    {"yummy",      0, "delicious",   2},
    {"delicious",  0, "delicious",   2},

    {NULL, 0, NULL, 0}
};

/* ---------------------------------------------------------------------------
 * Helpers
 * --------------------------------------------------------------------------- */

/**
 * @brief Simple tolower for ASCII only.
 * @param[in] c  Input character.
 * @return Lowercase variant if ASCII letter, otherwise unchanged.
 */
static inline char __ascii_lower(char c)
{
    return (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c;
}

/**
 * @brief Case-insensitive substring search for mixed UTF-8/ASCII text.
 *
 * Chinese characters have no case, so raw memcmp works for them.
 * For ASCII portions we do case-folded comparison.
 *
 * @param[in] haystack  String to search in.
 * @param[in] hlen      Length of haystack.
 * @param[in] needle    Keyword to find.
 * @param[in] nlen      Length of needle.
 * @return 1 if found, 0 otherwise.
 */
static int __contains(const char *haystack, int hlen,
                      const char *needle, int nlen)
{
    if (nlen <= 0 || hlen < nlen) {
        return 0;
    }
    for (int i = 0; i <= hlen - nlen; i++) {
        int match = 1;
        for (int j = 0; j < nlen; j++) {
            char h = haystack[i + j];
            char n = needle[j];
            if ((unsigned char)n < 0x80) {
                h = __ascii_lower(h);
                n = __ascii_lower(n);
            }
            if (h != n) {
                match = 0;
                break;
            }
        }
        if (match) {
            return 1;
        }
    }
    return 0;
}

/**
 * @brief Check if a byte is a UTF-8 sentence boundary character.
 *
 * Detects Chinese punctuation (UTF-8 encoded) and ASCII punctuation that
 * typically ends a sentence.
 *
 * @param[in] p    Pointer to the current byte.
 * @param[in] rem  Remaining bytes from p to end of buffer.
 * @return 1 if sentence boundary, 0 otherwise.
 */
static int __is_sentence_end(const char *p, int rem)
{
    unsigned char c = (unsigned char)*p;

    if (c == '.' || c == '!' || c == '?' || c == '~' || c == ',' || c == '\n') {
        return 1;
    }

    if (c == 0xE3 && rem >= 3) {
        unsigned char c1 = (unsigned char)p[1];
        unsigned char c2 = (unsigned char)p[2];
        /* U+3002 。 = E3 80 82 */
        if (c1 == 0x80 && c2 == 0x82) return 1;
    }

    if (c == 0xEF && rem >= 3) {
        unsigned char c1 = (unsigned char)p[1];
        unsigned char c2 = (unsigned char)p[2];
        /* U+FF01 ！ = EF BC 81 */
        if (c1 == 0xBC && c2 == 0x81) return 1;
        /* U+FF1F ？ = EF BC 9F */
        if (c1 == 0xBC && c2 == 0x9F) return 1;
        /* U+FF0C ， = EF BC 8C */
        if (c1 == 0xBC && c2 == 0x8C) return 1;
        /* U+FF1B ； = EF BC 9B */
        if (c1 == 0xBC && c2 == 0x9B) return 1;
        /* U+FF5E ～ = EF BD 9E */
        if (c1 == 0xBD && c2 == 0x9E) return 1;
    }

    if (c == 0xE2 && rem >= 3) {
        unsigned char c1 = (unsigned char)p[1];
        unsigned char c2 = (unsigned char)p[2];
        /* U+2026 … = E2 80 A6 */
        if (c1 == 0x80 && c2 == 0xA6) return 1;
    }

    return 0;
}

/**
 * @brief Get a lightweight monotonic tick count (ms).
 * @return Current tick in milliseconds.
 */
static unsigned int __get_ticks_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (unsigned int)(ts.tv_sec * 1000 + ts.tv_nsec / 1000000);
}

/**
 * @brief If in a keyword-triggered emotion and the decay timer expired,
 *        revert to the base emotion.
 * @return none
 */
static void __check_revert(void)
{
    if (!s_in_keyword_emo || s_revert_at_ms == 0) {
        return;
    }
    unsigned int now = __get_ticks_ms();
    if ((now - s_revert_at_ms) < KEYWORD_DECAY_MS) {
        return;
    }
    vrm_viewer_set_emotion(s_base_emo, 1.0f, 0.0f);
    s_current_emo    = s_base_emo;
    s_in_keyword_emo = 0;
    s_revert_at_ms   = 0;
    s_last_switch_ms = now;
}

/**
 * @brief Scan the sentence buffer for emotion keywords and apply the best match.
 * @param[in] sentence  Null-terminated sentence text.
 * @param[in] len       Length of the sentence.
 * @return none
 */
static void __scan_and_apply(const char *sentence, int len)
{
    if (len < MIN_SCAN_LEN) {
        return;
    }

    unsigned int now = __get_ticks_ms();
    if (s_last_switch_ms != 0 && (now - s_last_switch_ms) < EMOTION_COOLDOWN_MS) {
        return;
    }

    const char *best_emotion = NULL;
    int         best_priority = 0;

    for (int i = 0; s_rules[i].keyword != NULL; i++) {
        int klen = s_rules[i].kw_len;
        if (klen == 0) {
            klen = (int)strlen(s_rules[i].keyword);
            s_rules[i].kw_len = klen;
        }
        if (__contains(sentence, len, s_rules[i].keyword, klen)) {
            if (s_rules[i].priority > best_priority) {
                best_priority = s_rules[i].priority;
                best_emotion  = s_rules[i].emotion;
            }
        }
    }

    if (best_emotion && strcmp(best_emotion, s_current_emo) != 0) {
        vrm_viewer_set_emotion(best_emotion, 1.0f, 0.0f);
        s_current_emo    = best_emotion;
        s_last_switch_ms = now;
        if (strcmp(best_emotion, s_base_emo) != 0) {
            s_in_keyword_emo = 1;
            s_revert_at_ms   = now;
        } else {
            s_in_keyword_emo = 0;
            s_revert_at_ms   = 0;
        }
    }
}

/* ---------------------------------------------------------------------------
 * Function implementations
 * --------------------------------------------------------------------------- */

/**
 * @brief Reset the analyzer state. Call at the beginning of each LLM response.
 * @return none
 */
void text_emotion_reset(void)
{
    s_buf_len        = 0;
    s_buf[0]         = '\0';
    s_last_switch_ms = 0;
    s_current_emo    = "neutral";
    strncpy(s_base_emo, "neutral", BASE_EMO_MAX - 1);
    s_base_emo[BASE_EMO_MAX - 1] = '\0';
    s_in_keyword_emo = 0;
    s_revert_at_ms   = 0;
}

/**
 * @brief Set the base emotion from the LLM's MCP tool call.
 * @param[in] emotion  Emotion name. NULL resets to "neutral".
 * @return none
 */
void text_emotion_set_base(const char *emotion)
{
    if (!emotion || emotion[0] == '\0') {
        emotion = "neutral";
    }
    strncpy(s_base_emo, emotion, BASE_EMO_MAX - 1);
    s_base_emo[BASE_EMO_MAX - 1] = '\0';
    s_current_emo    = s_base_emo;
    s_in_keyword_emo = 0;
    s_revert_at_ms   = 0;
}

/**
 * @brief Feed a chunk of streaming text into the analyzer.
 * @param[in] text  UTF-8 text chunk.
 * @param[in] len   Length of the chunk in bytes.
 * @return none
 */
void text_emotion_feed(const char *text, int len)
{
    if (!text || len <= 0) {
        return;
    }

    __check_revert();

    for (int i = 0; i < len; i++) {
        if (s_buf_len < SENTENCE_BUF_SIZE - 1) {
            s_buf[s_buf_len++] = text[i];
        }

        int rem = len - i;
        if (__is_sentence_end(&text[i], rem)) {
            s_buf[s_buf_len] = '\0';
            __scan_and_apply(s_buf, s_buf_len);
            s_buf_len = 0;
        }

        if (s_buf_len >= SENTENCE_BUF_SIZE - 1) {
            s_buf[s_buf_len] = '\0';
            __scan_and_apply(s_buf, s_buf_len);
            s_buf_len = 0;
        }
    }
}

/**
 * @brief Flush remaining buffered text and run a final scan.
 * @return none
 */
void text_emotion_flush(void)
{
    if (s_buf_len > 0) {
        s_buf[s_buf_len] = '\0';
        __scan_and_apply(s_buf, s_buf_len);
        s_buf_len = 0;
    }

    if (s_in_keyword_emo) {
        vrm_viewer_set_emotion(s_base_emo, 1.0f, 0.0f);
        s_current_emo    = s_base_emo;
        s_in_keyword_emo = 0;
        s_revert_at_ms   = 0;
    }
}
