/*
*******************************************************************************
*
*   Copyright (C) 1998-2000, International Business Machines
*   Corporation and others.  All Rights Reserved.
*
*******************************************************************************
*
* File read.c
*
* Modification History:
*
*   Date        Name        Description
*   05/26/99    stephen     Creation.
*   5/10/01     Ram         removed ustdio dependency
*******************************************************************************
*/

#include "read.h"
#include "errmsg.h"
#include "unicode/ustring.h"

#define OPENBRACE    0x007B
#define CLOSEBRACE   0x007D
#define COMMA        0x002C
#define QUOTE        0x0022
#define ESCAPE       0x005C
#define SLASH        0x002F
#define ASTERISK     0x002A
#define SPACE        0x0020
#define COLON        0x003A
#define BADBOM       0xFFFE

static int32_t lineCount;

/* Protos */
static enum ETokenType getStringToken(UCHARBUF *buf,
                                      UChar32 initialChar,
                                      struct UString *token,
                                      UErrorCode *status);

static UChar32 getNextChar           (UCHARBUF *buf, UBool skipwhite, UErrorCode *status);
static void    seekUntilNewline      (UCHARBUF *buf, UErrorCode *status);
static void    seekUntilEndOfComment (UCHARBUF *buf, UErrorCode *status);
static UBool   isWhitespace          (UChar32 c);
static UBool   isNewline             (UChar32 c);

void resetLineNumber() {
    lineCount = 1;
}

/* Read and return the next token from the stream.  If the token is of
   type eString, fill in the token parameter with the token.  If the
   token is eError, then the status parameter will contain the
   specific error.  This will be eItemNotFound at the end of file,
   indicating that all tokens have been returned.  This method will
   never return eString twice in a row; instead, multiple adjacent
   string tokens will be merged into one, with no intervening
   space. */
enum ETokenType getNextToken(UCHARBUF* buf,
                             struct UString *token,
                             uint32_t *linenumber, /* out: linenumber of token */
                             UErrorCode *status) {
    enum ETokenType result;
    UChar32         c;

    if (U_FAILURE(*status)) {
        return TOK_ERROR;
    }

    /* Skip whitespace */
    c = getNextChar(buf, TRUE, status);

    if (U_FAILURE(*status)) {
        return TOK_ERROR;
    }

    *linenumber = lineCount;

    switch(c) {
    case BADBOM:
        return TOK_ERROR;
    case OPENBRACE:
        return TOK_OPEN_BRACE;
    case CLOSEBRACE:
        return TOK_CLOSE_BRACE;
    case COMMA:
        return TOK_COMMA;
    case U_EOF:
        return TOK_EOF;
    case COLON:
        return TOK_COLON;

    default:
        result = getStringToken(buf, c, token, status);
    }

    *linenumber = lineCount;
    return result;
}

/* Copy a string token into the given UnicodeString.  Upon entry, we
   have already read the first character of the string token, which is
   not a whitespace character (but may be a QUOTE or ESCAPE). This
   function reads all subsequent characters that belong with this
   string, and copy them into the token parameter. The other
   important, and slightly convoluted purpose of this function is to
   merge adjacent strings.  It looks forward a bit, and if the next
   non comment, non whitespace item is a string, it reads it in as
   well.  If two adjacent strings are quoted, they are merged without
   intervening space.  Otherwise a single SPACE character is
   inserted. */
static enum ETokenType getStringToken(UCHARBUF* buf,
                                      UChar32 initialChar,
                                      struct UString *token,
                                      UErrorCode *status) {
    UBool    lastStringWasQuoted;
    UChar32  c;
    UChar    target[3] = { '\0' };
    UChar    *pTarget   = target;
    int      len=0;
    UBool    isFollowingCharEscaped=FALSE;

    /* We are guaranteed on entry that initialChar is not a whitespace
       character. If we are at the EOF, or have some other problem, it
       doesn't matter; we still want to validly return the initialChar
       (if nothing else) as a string token. */

    if (U_FAILURE(*status)) {
        return TOK_ERROR;
    }

    /* setup */
    lastStringWasQuoted = FALSE;
    c = initialChar;
    ustr_setlen(token, 0, status);

    if (U_FAILURE(*status)) {
        return TOK_ERROR;
    }

    for (;;) {
        if (c == QUOTE) {
            if (!lastStringWasQuoted && token->fLength > 0) {
                ustr_ucat(token, SPACE, status);

                if (U_FAILURE(*status)) {
                    return TOK_ERROR;
                }
            }

            lastStringWasQuoted = TRUE;

            for (;;) {
                c = ucbuf_getc(buf,status);

                /* EOF reached */
                if (c == U_EOF) {
                    return TOK_EOF;
                }

                /* Unterminated quoted strings */
                if (U_FAILURE(*status)) {
                    return TOK_ERROR;
                }

                if (c == QUOTE && !isFollowingCharEscaped) {
                    break;
                }

                if (c == ESCAPE) {
                    pTarget = target;
                    c       = unescape(buf, status);

                    if (c == U_ERR) {
                        return TOK_ERROR;
                    }
                }               

                if(c==ESCAPE && !isFollowingCharEscaped){
                    isFollowingCharEscaped = TRUE;
                }else{
                    U_APPEND_CHAR32(c, pTarget,len);
                    pTarget = target;
                    ustr_uscat(token, pTarget,len, status);
                    isFollowingCharEscaped = FALSE;
                    len=0;
                }
                
                if (U_FAILURE(*status)) {
                    return TOK_ERROR;
                }
            }
        } else {
            if (token->fLength > 0) {
                ustr_ucat(token, SPACE, status);

                if (U_FAILURE(*status)) {
                    return TOK_ERROR;
                }
            }
            
            if(lastStringWasQuoted){
                if(getShowWarning()){
                    warning(lineCount, "Mixing quoted and unquoted strings");
                }
                if(isStrict()){
                    return TOK_ERROR;
                }

            }

            lastStringWasQuoted = FALSE;
            
            /* if we reach here we are mixing 
             * quoted and unquoted strings
             * warn in normal mode and error in
             * pedantic mode
             */

            if (c == ESCAPE) {
                pTarget = target;
                c       = unescape(buf, status);

                /* EOF reached */
                if (c == U_EOF) {
                    return TOK_ERROR;
                }
            }

            U_APPEND_CHAR32(c, pTarget,len);
            pTarget = target;
            ustr_uscat(token, pTarget,len, status);
            len=0;

            if (U_FAILURE(*status)) {
                return TOK_ERROR;
            }

            for (;;) {
                /* DON'T skip whitespace */
                c = getNextChar(buf, FALSE, status);

                /* EOF reached */
                if (c == U_EOF) {
                    ucbuf_ungetc(c, buf);
                    return TOK_STRING;
                }

                if (U_FAILURE(*status)) {
                    return TOK_STRING;
                }

                if (c == QUOTE
                        || c == OPENBRACE
                        || c == CLOSEBRACE
                        || c == COMMA
                        || c == COLON) {
                    ucbuf_ungetc(c, buf);
                    break;
                }

                if (isWhitespace(c)) {
                    break;
                }

                if (c == ESCAPE) {
                    pTarget = target;
                    c       = unescape(buf, status);

                    if (c == U_ERR) {
                        return TOK_ERROR;
                    }
                }

                U_APPEND_CHAR32(c, pTarget,len);
                pTarget = target;
                ustr_uscat(token, pTarget,len, status);
                len=0;
                if (U_FAILURE(*status)) {
                    return TOK_ERROR;
                }
            }
        }

        /* DO skip whitespace */
        c = getNextChar(buf, TRUE, status);

        if (U_FAILURE(*status)) {
            return TOK_STRING;
        }

        if (c == OPENBRACE || c == CLOSEBRACE || c == COMMA || c == COLON) {
            ucbuf_ungetc(c, buf);
            return TOK_STRING;
        }
    }
}

/* Retrieve the next character, ignoring comments.  If skipwhite is
   true, whitespace is skipped as well. */
static UChar32 getNextChar(UCHARBUF* buf,
                           UBool skipwhite,
                           UErrorCode *status) {
    UChar32 c;

    if (U_FAILURE(*status)) {
        return U_EOF;
    }

    for (;;) {
        c = ucbuf_getc(buf,status);

        if (c == U_EOF) {
            return U_EOF;
        }

        if (skipwhite && isWhitespace(c)) {
            continue;
        }

        /* This also handles the get() failing case */
        if (c != SLASH) {
            return c;
        }

        c = ucbuf_getc(buf,status);

        if (c == U_EOF) {
            return U_EOF;
        }

        switch (c) {
        case SLASH:
            seekUntilNewline(buf, status);
            break;

        case ASTERISK:
            seekUntilEndOfComment(buf, status);
            break;

        default:
            ucbuf_ungetc(c, buf);
            /* If get() failed this is a NOP */
            return SLASH;
        }
    }
}

static void seekUntilNewline(UCHARBUF* buf,
                             UErrorCode *status) {
    UChar32 c;

    if (U_FAILURE(*status)) {
        return;
    }

    do {
        c = ucbuf_getc(buf,status);
    } while (!isNewline(c) && c != U_EOF && *status == U_ZERO_ERROR);
}

static void seekUntilEndOfComment(UCHARBUF *buf,
                                  UErrorCode *status) {
    UChar32  c, d;
    uint32_t line;

    if (U_FAILURE(*status)) {
        return;
    }

    line = lineCount;

    do {
        c = ucbuf_getc(buf, status);

        if (c == ASTERISK) {
            d = ucbuf_getc(buf, status);

            if (d != SLASH) {
                ucbuf_ungetc(d, buf);
            } else {
                break;
            }
        }
    } while (c != U_EOF && *status == U_ZERO_ERROR);

    if (c == U_EOF) {
        *status = U_INVALID_FORMAT_ERROR;
        error(line, "unterminated comment detected");
    }
}

UChar32 unescape(UCHARBUF *buf,
                 UErrorCode *status) {
    if (U_FAILURE(*status)) {
        return U_EOF;
    }

    /* We expect to be called after the ESCAPE has been seen, but
     * u_fgetcx needs an ESCAPE to do its magic. */
    ucbuf_ungetc(ESCAPE, buf);

    return ucbuf_getcx32(buf, status);
}

static UBool isWhitespace(UChar32 c) {
    switch (c) {
        /* ' ', '\t', '\n', '\r', 0x2029, 0xFEFF */
    case 0x000A:
    case 0x2029:
        lineCount++;
    case 0x000D:
    case 0x0020:
    case 0x0009:
    case 0xFEFF:
        return TRUE;

    default:
        return FALSE;
    }
}

static UBool isNewline(UChar32 c) {
    switch (c) {
        /* '\n', '\r', 0x2029 */
    case 0x000A:
    case 0x2029:
        lineCount++;
    case 0x000D:
        return TRUE;

    default:
        return FALSE;
    }
}
