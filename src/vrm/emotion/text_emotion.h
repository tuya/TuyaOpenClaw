/**
 * @file text_emotion.h
 * @brief Real-time text emotion analyzer — scans streaming text for sentiment
 *        keywords and drives the avatar's facial expression automatically.
 * @version 1.0
 * @date 2025-03-29
 * @copyright Copyright (c) Tuya Inc.
 */
#ifndef __TEXT_EMOTION_H__
#define __TEXT_EMOTION_H__

#ifdef __cplusplus
extern "C" {
#endif

/* ---------------------------------------------------------------------------
 * Function declarations
 * --------------------------------------------------------------------------- */

/**
 * @brief Reset the analyzer state. Call at the beginning of each LLM response.
 * @return none
 */
void text_emotion_reset(void);

/**
 * @brief Set the base (overall mood) emotion established by the LLM's MCP call.
 *
 * Keyword-triggered emotions are temporary deviations.  After a short decay
 * period with no new keyword match, the expression automatically reverts to
 * this base emotion.
 *
 * @param[in] emotion  Emotion name (e.g. "happy", "sad"). NULL resets to "neutral".
 * @return none
 */
void text_emotion_set_base(const char *emotion);

/**
 * @brief Feed a chunk of streaming text into the analyzer.
 *
 * Internally buffers text until a sentence boundary is detected, then scans
 * for emotion keywords and calls vrm_viewer_set_emotion() when a match is
 * found. Safe to call from any thread that receives text stream events.
 *
 * @param[in] text  UTF-8 text chunk (not necessarily null-terminated).
 * @param[in] len   Length of the chunk in bytes.
 * @return none
 */
void text_emotion_feed(const char *text, int len);

/**
 * @brief Flush any remaining buffered text and run a final emotion scan.
 *        Call at the end of the text stream.
 * @return none
 */
void text_emotion_flush(void);

#ifdef __cplusplus
}
#endif

#endif /* __TEXT_EMOTION_H__ */
