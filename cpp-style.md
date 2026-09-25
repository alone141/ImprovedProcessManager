# C++ style

Follow these rules. Make new code look like the `LineParser` sample at the end of this document.

When a rule and the sample disagree about shape, the sample wins. When they disagree about what is allowed, this page wins. Short snippets below show shape only. They are not a second implementation.

## Language

Write C++20. `std::span`, ranges, and concepts are allowed.

## Files and includes

One concept per module. The file name matches the primary type in PascalCase.

- `include/LineParser.hpp` is the public API.
- `src/LineParser.cpp` is the implementation.
- `tests/LineParserTest.cpp` is that module's tests.
- `CMakeLists.txt`, `src/CMakeLists.txt`, and `tests/CMakeLists.txt` split the build.

`include/` is the public include root. Callers write `#include "LineParser.hpp"`. Headers are flat, with no namespace directory. Types that belong to the module share its header.

The header begins with `#pragma once`, then the system headers it needs, in alphabetical order.

```cpp
#pragma once

#include <optional>
#include <string>
#include <string_view>
```

A type that a header only names is forward-declared.

The source includes its own header first, then other project headers, a blank line, then system headers in alphabetical order.

```cpp
#include "LineParser.hpp"

#include <optional>
#include <string>
#include <string_view>
```

A test file includes GoogleTest, a blank line, then the header under test.

```cpp
#include <gtest/gtest.h>

#include "LineParser.hpp"
```

Helpers used only in the source sit in an anonymous namespace inside the project namespace.

## Names

| Kind | Spelling | Example |
|---|---|---|
| Namespace | snake_case | `line_parser` |
| Type or function | PascalCase | `LineParser`, `ParseLine` |
| Local or data member | camelCase | `limit`, `cached` |
| `constexpr` | snake_case | `max_line_length` |
| Macro | `SCREAMING_SNAKE_CASE` | `MAX_BUFFER_SIZE` |
| Enumerator | PascalCase | `Ok`, `EmptySource` |

An underscore appears in a `constexpr` name or a macro, and nowhere else. Members have no trailing underscore.

A function and its `enum class` are different identifiers. `ParseLine` returns `ParseCode`.

Use a macro only when one is required. Spell it `SCREAMING_SNAKE_CASE`.

## Format

Indent with 4 spaces. Put the opening brace on the next line for namespaces, types, functions, enums, and control statements. Put `public:` and `private:` one level inside the type. Put members and statements one level further in.

Bind `*` and `&` to the type. Brace-initialize objects. A short function still uses a full brace block.

```cpp
class LineParser
{
public:
    explicit LineParser(int limit);

    ParseCode ParseLine(std::string_view line, Line& out) const;

private:
    int limit;
};

ParseCode LineParser::ParseLine(std::string_view line, Line& out) const
{
    if (line.empty())
    {
        return ParseCode::EmptySource;
    }

    return ParseCode::Ok;
}
```

## Errors

A function that cannot fail returns its value.

A recoverable failure prefers an `enum class` return. The produced value is an out-parameter. Success is `Ok`.

```cpp
ParseCode ParseLine(std::string_view line, Line& out) const;
```

When the only failure is "not there," return `std::optional<T>`.

```cpp
std::optional<Line> FindCached(int id) const;
```

Another shape is allowed. Put a comment directly above the function that says why. That comment is required for every departure.

Exceptions stay rare. Catch a library that throws at the boundary and turn it into an `enum class`, unless a comment explains a different choice.

## API comments

Document every public function on its declaration in the header. Do not repeat that block on the definition.

Use a Doxygen block with a one-line `@brief`, one `@param` per parameter, and one `@return`. A constructor has `@brief` and `@param` and omits `@return`.

```cpp
/**
 * @brief Parse one line into @p out.
 * @param line Text to parse. An empty line fails.
 * @param out Receives the parsed line on success.
 * @return ParseCode::Ok on success, otherwise the failure.
 */
ParseCode ParseLine(std::string_view line, Line& out) const;
```

A private helper has no comment. Add a comment only when the name cannot carry the reason.

A function that departs from the preferred error shape still has the Doxygen block. The reason for the other shape is a separate comment, placed directly above that block.

## Tests

Use GoogleTest. The suite has one runner, `gtest_main`. A test file does not define `main`.

Each module has one test file named after the type: `tests/LineParserTest.cpp`.

A case is a `TEST`. The first argument is the fixture name. The second is the case name. Both are PascalCase, and the case name says what the behavior is.

`EXPECT_*` records a failure and the case keeps going. `ASSERT_*` is for a check where the rest of the case would be meaningless. Test cases do not get a Doxygen block.

```cpp
TEST(LineParserTest, EmptyLineReturnsEmptySource)
{
    line_parser::LineParser parser{line_parser::max_line_length};
    line_parser::Line out{};
    EXPECT_EQ(parser.ParseLine("", out), line_parser::ParseCode::EmptySource);
}
```

## Resources

Owning memory is an RAII object, usually `std::unique_ptr`. A type that only holds values and RAII members stays copyable under the Rule of Zero. A type that owns a resource and must not be copied deletes copy and defines move.

Non-owning text is `std::string_view`. A non-owning contiguous sequence is `std::span`.

A single-argument constructor is `explicit`. A local that does not change is `const`. A method whose value result does not depend on later mutation of the object is `const`. A cache written as a side effect of a successful call is `mutable`.

Use `auto` only when the initializer already shows the type.

```cpp
const Line* found = cached.has_value() ? &cached.value() : nullptr;
const auto* current = found;
```

## Build

Split the build. The root `CMakeLists.txt` owns project settings. Each subdirectory `CMakeLists.txt` owns the targets in that directory. Paths in a subdirectory file are relative to that directory.

The root file sets:

- `cmake_minimum_required` and `project`
- C++20, with the standard required and compiler extensions off
- project-wide compile settings
- `enable_testing`
- the directory list, through `add_subdirectory`

A subdirectory file sets the specifics of its targets: sources, include directories, link libraries, compile definitions, and compile options. `PUBLIC` is for the API. `PRIVATE` is for the implementation and for tests. A dependency used by one directory is found in that directory. A subdirectory does not call `project`, and it does not change the language standard.

`CMakeLists.txt`

```cmake
cmake_minimum_required(VERSION 3.20)
project(line_parser LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_subdirectory(src)

enable_testing()
add_subdirectory(tests)
```

`src/CMakeLists.txt`

```cmake
add_library(line_parser LineParser.cpp)
target_include_directories(line_parser PUBLIC ${PROJECT_SOURCE_DIR}/include)
```

`tests/CMakeLists.txt`

```cmake
find_package(GTest REQUIRED)

include(GoogleTest)
add_executable(line_parser_tests LineParserTest.cpp)
target_link_libraries(line_parser_tests PRIVATE line_parser GTest::gtest_main)
gtest_discover_tests(line_parser_tests)
```

Configure, build, and run the tests that are already on the machine:

```text
cmake -S . -B build
cmake --build build
ctest --test-dir build
```

## Airgapped machines

Configure and build with no network. Every dependency is already installed, and found with `find_package`, or it is vendored in the tree and added with `add_subdirectory` (for example `third_party/googletest`).

`FetchContent`, `ExternalProject`, `file(DOWNLOAD)`, and a configure step that clones or curls a URL are out of bounds. A missing package fails configure with the package name and the version to install offline. It does not offer a download.

A vendored dependency keeps its own `CMakeLists.txt`. The root file adds that directory before the targets that link it. Record packages and versions in the root `CMakeLists.txt` or in `third_party/README`. `ctest` runs the binaries already built. It does not download test data.

## Sample

`LineParser` parses one line. It is the shape to copy. It lives in namespace `line_parser`.

`include/LineParser.hpp`

```cpp
#pragma once

#include <optional>
#include <string>
#include <string_view>

namespace line_parser
{

constexpr int max_line_length = 64;
constexpr int cached_id = 0;

struct Line
{
    std::string text;
};

enum class ParseCode
{
    Ok,
    EmptySource,
    TooLong,
};

class LineParser
{
public:
    /**
     * @brief Store the maximum accepted line length in bytes.
     * @param limit Maximum line length in bytes.
     */
    explicit LineParser(int limit);

    /**
     * @brief Parse one line into @p out.
     * @param line Text to parse. An empty line fails.
     * @param out Receives the parsed line on success.
     * @return ParseCode::Ok on success, otherwise the failure.
     */
    ParseCode ParseLine(std::string_view line, Line& out) const;

    /**
     * @brief Return the last line successfully parsed.
     * @param id Cache id. Only cached_id can hit.
     * @return The cached line, or std::nullopt on a miss.
     */
    std::optional<Line> FindCached(int id) const;

    // The caller only branches on success, so a bool is enough.
    /**
     * @brief Accept a line the parser would parse successfully.
     * @param line Text to parse. An empty line fails.
     * @param out Receives the parsed line on success.
     * @return true when parsing would return Ok.
     */
    bool AcceptLine(std::string_view line, Line& out) const;

private:
    int limit;
    // mutable: a successful parse records the last line without changing parse results.
    mutable std::optional<Line> cached;
};

} // namespace line_parser
```

`src/LineParser.cpp`

```cpp
#include "LineParser.hpp"

#include <optional>
#include <string>
#include <string_view>

namespace line_parser
{

namespace
{

bool IsTooLong(std::string_view line, int limit)
{
    if (limit < 0)
    {
        return true;
    }

    return line.size() > static_cast<std::size_t>(limit);
}

} // namespace

LineParser::LineParser(int limit)
    : limit{limit}
{
}

ParseCode LineParser::ParseLine(std::string_view line, Line& out) const
{
    if (line.empty())
    {
        return ParseCode::EmptySource;
    }

    if (IsTooLong(line, limit))
    {
        return ParseCode::TooLong;
    }

    out = Line{std::string{line}};
    cached = out;
    return ParseCode::Ok;
}

std::optional<Line> LineParser::FindCached(int id) const
{
    const Line* found = cached.has_value() ? &cached.value() : nullptr;
    const auto* current = found;
    if (current == nullptr || id != cached_id)
    {
        return std::nullopt;
    }

    return *current;
}

bool LineParser::AcceptLine(std::string_view line, Line& out) const
{
    return ParseLine(line, out) == ParseCode::Ok;
}

} // namespace line_parser
```

Behavior the sample implements:

- An empty line returns `EmptySource`. `out` and the cache stay unchanged.
- A line longer than `limit` returns `TooLong`. A line whose size equals `limit` is accepted. A negative `limit` makes every non-empty line `TooLong`.
- Any other line copies the text into `out`, stores that line in the cache, and returns `Ok`.
- `FindCached(cached_id)` returns the cached line after a success. Any other id, or a call before the first success, returns `std::nullopt`. A failed parse does not clear the cache.
- `AcceptLine` returns whether `ParseLine` returned `Ok`.

`tests/LineParserTest.cpp`

```cpp
#include <gtest/gtest.h>

#include "LineParser.hpp"

#include <string>

TEST(LineParserTest, ParsesShortLine)
{
    line_parser::LineParser parser{line_parser::max_line_length};
    line_parser::Line out{};
    EXPECT_EQ(parser.ParseLine("abc", out), line_parser::ParseCode::Ok);
    EXPECT_EQ(out.text, "abc");
}

TEST(LineParserTest, EmptyLineReturnsEmptySource)
{
    line_parser::LineParser parser{line_parser::max_line_length};
    line_parser::Line out{};
    EXPECT_EQ(parser.ParseLine("", out), line_parser::ParseCode::EmptySource);
}

TEST(LineParserTest, LongLineReturnsTooLong)
{
    line_parser::LineParser parser{line_parser::max_line_length};
    line_parser::Line out{};
    const std::string line(static_cast<std::size_t>(line_parser::max_line_length) + 1, 'x');
    EXPECT_EQ(parser.ParseLine(line, out), line_parser::ParseCode::TooLong);
}

TEST(LineParserTest, ReturnsCachedLineAfterSuccess)
{
    line_parser::LineParser parser{line_parser::max_line_length};
    line_parser::Line out{};
    EXPECT_EQ(parser.ParseLine("abc", out), line_parser::ParseCode::Ok);
    const std::optional<line_parser::Line> cached = parser.FindCached(line_parser::cached_id);
    ASSERT_TRUE(cached.has_value());
    EXPECT_EQ(cached->text, "abc");
}

TEST(LineParserTest, UnknownIdReturnsNullopt)
{
    line_parser::LineParser parser{line_parser::max_line_length};
    line_parser::Line out{};
    EXPECT_EQ(parser.ParseLine("abc", out), line_parser::ParseCode::Ok);
    EXPECT_FALSE(parser.FindCached(line_parser::cached_id + 1).has_value());
}

TEST(LineParserTest, AcceptLineAcceptsShortLine)
{
    line_parser::LineParser parser{line_parser::max_line_length};
    line_parser::Line out{};
    EXPECT_TRUE(parser.AcceptLine("abc", out));
}

TEST(LineParserTest, AcceptLineRejectsEmptyLine)
{
    line_parser::LineParser parser{line_parser::max_line_length};
    line_parser::Line out{};
    EXPECT_FALSE(parser.AcceptLine("", out));
}
```
