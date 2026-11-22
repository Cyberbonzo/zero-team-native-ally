/* board-side glue for Zero Team's save-state, included in the single-title
 * board translation unit after its declarations. This file is NOT part of the
 * frontend build; it is kept next to the frontend so the board and the
 * frontend agree on the on-disk contract. FBNeo scans stack temporaries as
 * well as persistent data, so copy each BurnArea while the callback is active;
 * its address is not necessarily valid afterwards. */
namespace {
struct ZtStateHeader {
    uint32_t magic;
    uint32_t version;
    uint32_t frame;
    uint32_t layout;
    uint32_t data_crc;
};
constexpr uint32_t state_magic = 0x5A545354u;
struct StateScan {
    uint8_t *output = nullptr;
    const uint8_t *input = nullptr;
    size_t capacity = 128 * 1024 * 1024;
    size_t used = 0;
    uint32_t layout = 0;
    bool valid = true;
};
StateScan *current_scan = nullptr;

INT32 __cdecl scan_area(BurnArea *area) {
    auto &scan = *current_scan;
    if (!scan.valid || !area || (!area->Data && area->nLen) || area->nLen > scan.capacity - scan.used) {
        scan.valid = false;
        return 1;
    }
    scan.layout = crc32(scan.layout, reinterpret_cast<const Bytef *>(&area->nLen), sizeof(area->nLen));
    if (area->szName) {
        scan.layout = crc32(scan.layout, reinterpret_cast<const Bytef *>(area->szName), strlen(area->szName) + 1);
    }
    if (area->nLen) {
        if (scan.output) memcpy(scan.output + scan.used, area->Data, area->nLen);
        if (scan.input) memcpy(area->Data, scan.input + scan.used, area->nLen);
    }
    scan.used += area->nLen;
    return 0;
}

bool scan_board(StateScan &scan) {
    if (!active || current_scan) return false;
    const auto previous = BurnAcb;
    current_scan = &scan;
    BurnAcb = scan_area;
    const int action = ACB_FULLSCAN | (scan.input ? ACB_WRITE : ACB_READ);
    int minimum = 0;
    const auto result = BurnDrvZeroTeam.AreaScan(action, &minimum);
    BurnAcb = previous;
    current_scan = nullptr;
    return !result && scan.valid && scan.used != 0;
}
}

size_t zt_state_size() {
    StateScan scan;
    return scan_board(scan) ? sizeof(ZtStateHeader) + scan.used : 0;
}

int zt_save_state(void *out, size_t size) {
    if (!out || size <= sizeof(ZtStateHeader)) return -1;
    StateScan scan;
    scan.output = static_cast<uint8_t *>(out) + sizeof(ZtStateHeader);
    scan.capacity = size - sizeof(ZtStateHeader);
    if (!scan_board(scan) || scan.used != scan.capacity) return -1;
    const ZtStateHeader header{state_magic, 1, nCurrentFrame, scan.layout,
                               static_cast<uint32_t>(crc32(0, scan.output, scan.used))};
    memcpy(out, &header, sizeof(header));
    return 0;
}

int zt_load_state(const void *data, size_t size) {
    if (!data || size <= sizeof(ZtStateHeader)) return -1;
    ZtStateHeader header{};
    memcpy(&header, data, sizeof(header));
    StateScan survey;
    if (header.magic != state_magic || header.version != 1 || !scan_board(survey) ||
        header.layout != survey.layout || size - sizeof(header) != survey.used)
        return -1;
    const auto *payload = static_cast<const uint8_t *>(data) + sizeof(header);
    if (crc32(0, payload, survey.used) != header.data_crc) return -1;

    std::vector<uint8_t> previous(survey.used);
    StateScan before;
    before.output = previous.data();
    before.capacity = previous.size();
    if (!scan_board(before) || before.used != survey.used) return -1;
    StateScan load;
    load.input = payload;
    load.capacity = survey.used;
    if (!scan_board(load) || load.used != survey.used || load.layout != survey.layout) {
        StateScan restore;
        restore.input = previous.data();
        restore.capacity = previous.size();
        scan_board(restore);
        return -1;
    }
    nCurrentFrame = header.frame;
    if (BurnDrvZeroTeam.pRecalcPal) *BurnDrvZeroTeam.pRecalcPal = 1;
    return 0;
}
