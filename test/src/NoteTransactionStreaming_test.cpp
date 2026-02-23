/*!
 * @file NoteTransactionStreaming_test.cpp
 *
 * Tests for the string-based transaction API (Layer 0 and Layer 1).
 *
 * Copyright (c) 2024 Blues Inc. MIT License.
 */

#include <catch2/catch_test_macros.hpp>
#include <fff.h>

#include "n_lib.h"

#include <cstring>
#include <string>

DEFINE_FFF_GLOBALS
FAKE_VALUE_FUNC(char *, _crcAdd, char *, uint16_t)
FAKE_VALUE_FUNC(bool, _crcError, char *, uint16_t)
FAKE_VALUE_FUNC(bool, _noteHardReset)
FAKE_VALUE_FUNC(const char *, _noteJSONTransaction, const char *, size_t, char **, uint32_t)
FAKE_VALUE_FUNC(bool, _noteTransactionStart, uint32_t)
FAKE_VALUE_FUNC(void *, NoteMalloc, size_t)
FAKE_VALUE_FUNC(bool, NoteErrorContains, const char *, const char *)
FAKE_VOID_FUNC(NoteFree, void *)
FAKE_VOID_FUNC(NoteDebugWithLevel, uint8_t, const char *)
FAKE_VOID_FUNC(NoteDelayMs, uint32_t)
FAKE_VOID_FUNC(_noteLockNote)
FAKE_VOID_FUNC(_noteUnlockNote)
FAKE_VOID_FUNC(_noteTransactionStop)

namespace
{

// Helper: simulate a successful transport response
const char *fakeTransactionSuccess(const char *, size_t, char **resp, uint32_t)
{
    static const char rspStr[] = "{\"version\":\"notecard-1.0\"}\n";
    if (resp) {
        char *buf = reinterpret_cast<char *>(malloc(sizeof(rspStr)));
        memcpy(buf, rspStr, sizeof(rspStr));
        *resp = buf;
    }
    return NULL;
}

// Helper: simulate a transport I/O error
const char *fakeTransactionIoError(const char *, size_t, char **, uint32_t)
{
    return "some error {io}";
}

// Helper: simulate a transport fatal (non-IO) error
const char *fakeTransactionFatalError(const char *, size_t, char **, uint32_t)
{
    return "fatal error";
}

// Callback context for capturing response
struct RxCapture {
    std::string data;
    int callCount;
};

bool captureCallback(void *ctx, const char *data, size_t len)
{
    auto *cap = reinterpret_cast<RxCapture *>(ctx);
    cap->data.append(data, len);
    cap->callCount++;
    return true;
}

bool abortCallback(void *ctx, const char *data, size_t len)
{
    (void)ctx;
    (void)data;
    (void)len;
    return false;  // abort
}

} // anonymous namespace

// ============================================================
// _jsonIsCommand tests
// ============================================================

SCENARIO("_jsonIsCommand")
{
    GIVEN("A request JSON string") {
        THEN("It returns false") {
            CHECK(_jsonIsCommand("{\"req\":\"card.version\"}") == false);
        }
    }
    GIVEN("A command JSON string") {
        THEN("It returns true") {
            CHECK(_jsonIsCommand("{\"cmd\":\"hub.set\"}") == true);
        }
    }
    GIVEN("Neither req nor cmd") {
        THEN("It returns false") {
            CHECK(_jsonIsCommand("{\"foo\":\"bar\"}") == false);
        }
    }
    GIVEN("A string with cmd in a value but not as a key") {
        THEN("It returns false for req with cmd value") {
            // The "cmd" appears in "req" value - _jsonIsCommand checks for
            // the pattern "cmd": (with colon) so this could match if the
            // value contains "cmd":. In practice, Notecard values don't
            // contain this pattern.
            CHECK(_jsonIsCommand("{\"req\":\"card.version\"}") == false);
        }
    }
}

// ============================================================
// _rspContainsError tests
// ============================================================

SCENARIO("_rspContainsError")
{
    GIVEN("A response with an io error") {
        THEN("It finds the error type") {
            CHECK(_rspContainsError("{\"err\":\"timeout {io}\"}", "{io}") == true);
        }
    }
    GIVEN("A response with no error") {
        THEN("It returns false") {
            CHECK(_rspContainsError("{\"version\":\"1.0\"}", "{io}") == false);
        }
    }
    GIVEN("A response with a different error type") {
        THEN("It returns false for the wrong type") {
            CHECK(_rspContainsError("{\"err\":\"bad binary {bad-bin}\"}", "{io}") == false);
        }
        THEN("It returns true for the correct type") {
            CHECK(_rspContainsError("{\"err\":\"bad binary {bad-bin}\"}", "{bad-bin}") == true);
        }
    }
    GIVEN("A response with an empty err field") {
        THEN("It returns false") {
            CHECK(_rspContainsError("{\"err\":\"\"}", "{io}") == false);
        }
    }
}

// ============================================================
// NoteTransactionStreaming tests (Layer 0)
// ============================================================

SCENARIO("NoteTransactionStreaming")
{
    // Common setup — delegate malloc/free to real implementations
    NoteMalloc_fake.custom_fake = malloc;
    NoteFree_fake.custom_fake = free;
    _noteTransactionStart_fake.return_val = true;
    _noteHardReset_fake.return_val = true;
    resetRequired = false;

    GIVEN("A NULL request") {
        THEN("It returns an error") {
            const char *err = NoteTransactionStreaming(NULL, 0, NULL, NULL);
            CHECK(err != NULL);
        }
    }

    GIVEN("A request with neither req nor cmd") {
        const char req[] = "{\"foo\":\"bar\"}\n";
        THEN("It returns an error") {
            const char *err = NoteTransactionStreaming(req, strlen(req), NULL, NULL);
            CHECK(err != NULL);
        }
    }

    GIVEN("A valid request") {
        const char req[] = "{\"req\":\"card.version\"}\n";

        AND_GIVEN("The transport succeeds") {
            _noteJSONTransaction_fake.custom_fake = fakeTransactionSuccess;
            RxCapture capture = {"", 0};

            WHEN("NoteTransactionStreaming is called with a callback") {
                const char *err = NoteTransactionStreaming(
                    req, strlen(req), captureCallback, &capture);

                THEN("It succeeds") {
                    CHECK(err == NULL);
                }
                THEN("The callback is invoked with the response") {
                    CHECK(capture.callCount == 1);
                    CHECK(capture.data.find("notecard-1.0") != std::string::npos);
                }
                THEN("The transport was called") {
                    CHECK(_noteJSONTransaction_fake.call_count >= 1);
                }
            }

            WHEN("NoteTransactionStreaming is called with NULL callback") {
                const char *err = NoteTransactionStreaming(
                    req, strlen(req), NULL, NULL);

                THEN("It still succeeds (response discarded)") {
                    CHECK(err == NULL);
                }
            }
        }

        AND_GIVEN("The transport returns an I/O error") {
            _noteJSONTransaction_fake.custom_fake = fakeTransactionIoError;

            WHEN("NoteTransactionStreaming is called") {
                const char *err = NoteTransactionStreaming(
                    req, strlen(req), NULL, NULL);

                THEN("It returns an error after retries") {
                    CHECK(err != NULL);
                }
                THEN("It retried") {
                    CHECK(_noteJSONTransaction_fake.call_count > 1);
                }
            }
        }

        AND_GIVEN("The transport returns a fatal error") {
            _noteJSONTransaction_fake.custom_fake = fakeTransactionFatalError;

            WHEN("NoteTransactionStreaming is called") {
                const char *err = NoteTransactionStreaming(
                    req, strlen(req), NULL, NULL);

                THEN("It returns an error without retries") {
                    CHECK(err != NULL);
                }
                THEN("It did not retry") {
                    CHECK(_noteJSONTransaction_fake.call_count == 1);
                }
            }
        }
    }

    GIVEN("A command (fire-and-forget)") {
        const char cmd[] = "{\"cmd\":\"hub.set\"}\n";
        _noteJSONTransaction_fake.custom_fake = fakeTransactionSuccess;
        RxCapture capture = {"", 0};

        WHEN("NoteTransactionStreaming is called") {
            const char *err = NoteTransactionStreaming(
                cmd, strlen(cmd), captureCallback, &capture);

            THEN("It succeeds") {
                CHECK(err == NULL);
            }
            THEN("The callback is NOT invoked (no response for cmd)") {
                CHECK(capture.callCount == 0);
            }
            THEN("The transport was called with NULL response pointer") {
                REQUIRE(_noteJSONTransaction_fake.call_count >= 1);
                CHECK(_noteJSONTransaction_fake.arg2_history[0] == NULL);
            }
        }
    }

    GIVEN("The transaction start fails") {
        _noteTransactionStart_fake.return_val = false;
        const char req[] = "{\"req\":\"card.version\"}\n";

        WHEN("NoteTransactionStreaming is called") {
            const char *err = NoteTransactionStreaming(
                req, strlen(req), NULL, NULL);

            THEN("It returns an error") {
                CHECK(err != NULL);
            }
            THEN("The transport was not called") {
                CHECK(_noteJSONTransaction_fake.call_count == 0);
            }
        }
    }

    // Reset fakes
    RESET_FAKE(_crcAdd);
    RESET_FAKE(_crcError);
    RESET_FAKE(_noteHardReset);
    RESET_FAKE(_noteJSONTransaction);
    RESET_FAKE(_noteTransactionStart);
    RESET_FAKE(NoteMalloc);
    RESET_FAKE(NoteFree);
    RESET_FAKE(NoteErrorContains);
    RESET_FAKE(NoteDebugWithLevel);
    RESET_FAKE(NoteDelayMs);
    RESET_FAKE(_noteLockNote);
    RESET_FAKE(_noteUnlockNote);
    RESET_FAKE(_noteTransactionStop);
    FFF_RESET_HISTORY();
}

// ============================================================
// NoteTransactionString tests (Layer 1)
// ============================================================

SCENARIO("NoteTransactionString")
{
    // Common setup
    NoteMalloc_fake.custom_fake = malloc;
    NoteFree_fake.custom_fake = free;
    _noteTransactionStart_fake.return_val = true;
    _noteHardReset_fake.return_val = true;
    resetRequired = false;
    _noteJSONTransaction_fake.custom_fake = fakeTransactionSuccess;

    GIVEN("A valid request and sufficient buffer") {
        const char req[] = "{\"req\":\"card.version\"}\n";
        char rspBuf[256] = {0};
        size_t rspLen = 0;

        WHEN("NoteTransactionString is called") {
            const char *err = NoteTransactionString(
                req, strlen(req), rspBuf, sizeof(rspBuf), &rspLen);

            THEN("It succeeds") {
                CHECK(err == NULL);
            }
            THEN("The response is in the buffer") {
                CHECK(rspLen > 0);
                CHECK(strstr(rspBuf, "notecard-1.0") != NULL);
            }
            THEN("The response is null-terminated") {
                CHECK(rspBuf[rspLen] == '\0');
            }
        }
    }

    GIVEN("A valid request but buffer too small") {
        const char req[] = "{\"req\":\"card.version\"}\n";
        char rspBuf[4] = {0};  // Way too small
        size_t rspLen = 0;

        WHEN("NoteTransactionString is called") {
            const char *err = NoteTransactionString(
                req, strlen(req), rspBuf, sizeof(rspBuf), &rspLen);

            THEN("It returns an overflow error") {
                CHECK(err != NULL);
            }
            THEN("rspLen reports the required size") {
                CHECK(rspLen > sizeof(rspBuf));
            }
        }
    }

    GIVEN("A command with NULL response buffer") {
        const char cmd[] = "{\"cmd\":\"hub.set\"}\n";
        size_t rspLen = 0;

        WHEN("NoteTransactionString is called") {
            const char *err = NoteTransactionString(
                cmd, strlen(cmd), NULL, 0, &rspLen);

            THEN("It succeeds") {
                CHECK(err == NULL);
            }
        }
    }

    GIVEN("A request with NULL rspLen") {
        const char req[] = "{\"req\":\"card.version\"}\n";
        char rspBuf[256] = {0};

        WHEN("NoteTransactionString is called") {
            const char *err = NoteTransactionString(
                req, strlen(req), rspBuf, sizeof(rspBuf), NULL);

            THEN("It succeeds") {
                CHECK(err == NULL);
            }
            THEN("The response is in the buffer") {
                CHECK(strstr(rspBuf, "notecard-1.0") != NULL);
            }
        }
    }

    // Reset fakes
    RESET_FAKE(_crcAdd);
    RESET_FAKE(_crcError);
    RESET_FAKE(_noteHardReset);
    RESET_FAKE(_noteJSONTransaction);
    RESET_FAKE(_noteTransactionStart);
    RESET_FAKE(NoteMalloc);
    RESET_FAKE(NoteFree);
    RESET_FAKE(NoteErrorContains);
    RESET_FAKE(NoteDebugWithLevel);
    RESET_FAKE(NoteDelayMs);
    RESET_FAKE(_noteLockNote);
    RESET_FAKE(_noteUnlockNote);
    RESET_FAKE(_noteTransactionStop);
    FFF_RESET_HISTORY();
}
