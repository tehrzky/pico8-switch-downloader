#pragma once
#include <string>
#include <vector>

// One cartridge entry, as scraped from the Lexaloffle BBS.
struct CartItem {
    std::string tid;             // BBS thread id
    std::string title;
    std::string author;          // filled in lazily by resolve_cart_detail()
    std::string thread_url;      // human-readable BBS page for this cart
    std::string download_url;    // direct .p8.png link, filled in lazily
    std::string thumbnail_url;   // preview image link, filled in lazily
    bool detail_resolved = false; // true once we've tried to fill in the fields above
    bool downloading = false;
};

enum class CartFilter {
    Featured,
    New,
    Popular,
    TopRated
};

// Fetches one page of the "cat=7" (PICO-8) cart listing from the BBS for the given
// filter/search/page. Returns true if the network request itself succeeded (an empty
// result set is not an error). On failure, out_error is filled with a human-readable
// message you can show on screen.
bool fetch_cart_list(CartFilter filter,
                      const std::string& search,
                      int page,
                      std::vector<CartItem>& out_items,
                      std::string& out_error);

// Visits the individual thread page for a cart and tries to fill in author,
// download_url and thumbnail_url. Safe to call even if the site layout has
// changed - it will just leave the fields blank instead of crashing.
void resolve_cart_detail(CartItem& item);

// Downloads a small binary blob (used for thumbnail images) into memory.
bool http_get_binary(const std::string& url, std::vector<unsigned char>& out);
