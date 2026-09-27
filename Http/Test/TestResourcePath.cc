#include "base/Util.hpp"
#include <cassert>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>

namespace fs = std::filesystem;

struct Fixture
{
    fs::path base;
    Fixture()
    {
        char pattern[] = "/tmp/http-resource-path-XXXXXX";
        const char *directory = ::mkdtemp(pattern);
        if (!directory)
            throw std::runtime_error("mkdtemp failed");
        base = directory;
    }
    ~Fixture()
    {
        std::error_code ec;
        fs::remove_all(base, ec);
    }
};

int main()
{
    Fixture fixture;
    const auto root = fixture.base / "www";
    const auto outside = fixture.base / "www-other";
    fs::create_directories(root / "assets");
    fs::create_directory(outside);
    std::ofstream(root / "index.html") << "hello";
    std::ofstream(root / "assets/image.png") << "image";
    fs::create_directory(root / "%2e%2e");
    std::ofstream(root / "%2e%2e/file") << "literal percent encoding";
    std::ofstream(outside / "secret") << "outside";
    std::ofstream(fixture.base / "secret") << "outside parent";
    fs::create_symlink(root / "absent", root / "dangling");
    fs::create_directory_symlink(root / "assets", root / "inside");
    fs::create_directory_symlink(outside, root / "outside");
    fs::create_directory_symlink(root, fixture.base / "root-link");
    fs::create_symlink("loop", root / "loop");

    const auto accept = [&](std::string_view input, const fs::path &expected) {
        fs::path result = "unchanged";
        assert(Util::ResolveResourcePath(root, input, result));
        assert(result == expected);
    };
    const auto reject = [&](const fs::path &document_root, std::string_view input) {
        fs::path result = "unchanged";
        assert(!Util::ResolveResourcePath(document_root, input, result));
        assert(result == "unchanged");
    };

    accept("/", root);
    accept("/index.html", root / "index.html");
    accept("//assets///image.png", root / "assets/image.png");
    accept("/assets/../index.html", root / "index.html");
    accept("/assets", root / "assets");
    reject(root, "/missing/file.txt");
    reject(root, "/assets/missing.png");
    reject(root, "/dangling");
    accept("/inside/image.png", root / "assets/image.png");
    accept("/%2e%2e/file", root / "%2e%2e/file"); // 不重复解码。
    reject(root, "");
    reject(root, "index.html");
    reject(root, std::string("/a\0b", 4));
    reject(root, "/../www-other/secret");
    reject(root, "/../../secret");
    reject(root, "/outside/secret");
    reject(root, "/outside/../secret");
    reject(root, "/loop/file");
    reject(root / "absent", "/index.html");
    reject(root / "index.html", "/index.html");
    reject({}, "/index.html");

    fs::path result;
    assert(Util::ResolveResourcePath(fixture.base / "root-link", "/index.html", result));
    assert(result == root / "index.html");
    assert(Util::ResolveResourcePath(fs::relative(root), "/index.html", result));
    assert(result == root / "index.html");
    result = root;
    assert(Util::ResolveResourcePath(result, "/index.html", result));
    assert(result == root / "index.html");
    std::cout << "Resource path tests passed\n";
}
