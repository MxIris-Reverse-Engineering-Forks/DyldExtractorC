#include "LinkeditOptimizer.h"

#include <format>
#include <map>
#include <spdlog/spdlog.h>
#include <string_view>

using namespace Converter;

template <class P>
LinkeditTracker<P>::LinkeditTracker(Macho::Context<false, P> &mCtx)
    : _header(mCtx.header) {
    auto textSect = mCtx.getSection("__TEXT", "__text");
    if (!textSect) {
        throw std::invalid_argument(
            "Mach-O Context doesn't have a __text sect.");
    }
    auto textSectStart = mCtx.convertAddrP(textSect->addr);
    _commandsStart =
        (uint8_t *)_header + sizeof(Macho::Context<false, P>::HeaderT);
    _headerSpaceAvailable = (uint32_t)(textSectStart - _commandsStart);

    auto linkeditSeg = mCtx.getSegment("__LINKEDIT");
    if (!linkeditSeg) {
        throw std::invalid_argument(
            "Mach-O Context doesn't have a __LINKEDIT segment.");
    }
    _linkeditStart = mCtx.convertAddrP(linkeditSeg->command->vmaddr);
    _linkeditEnd = _linkeditStart + linkeditSeg->command->vmsize;
}

template <class P>
bool LinkeditTracker<P>::insertLoadCommand(Macho::Loader::load_command *after,
                                           Macho::Loader::load_command *lc) {
    // Make sure there is enough space
    if (_header->sizeofcmds + lc->cmdsize > _headerSpaceAvailable) {
        return false;
    }

    // Move all load commands after `after`
    std::size_t shiftDelta = lc->cmdsize;
    uint8_t *shiftStart = (uint8_t *)after + after->cmdsize;
    uint8_t *shiftEnd = _commandsStart + _header->sizeofcmds;
    memmove(shiftStart + shiftDelta, shiftStart, shiftEnd - shiftStart);

    memcpy(shiftStart, lc, lc->cmdsize);

    // Adjust tracking pointers
    for (auto &ptrPair : trackingData) {
        if (ptrPair.offset >= shiftStart) {
            ptrPair.offset += shiftDelta;
        }
    }

    // Adjust header
    _header->ncmds++;
    _header->sizeofcmds += lc->cmdsize;
    return true;
}

template <class P>
bool LinkeditTracker<P>::insertLinkeditData(std::optional<LinkeditData> after,
                                            LinkeditData data) {
    // calculate shift amount with pointer align
    uint32_t shiftDelta = data.dataSize + (8 - (data.dataSize % 8));

    // Check that there is enough space
    uint8_t *lastDataEnd;
    if (trackingData.size()) {
        auto lastData = *trackingData.rbegin();
        lastDataEnd = lastData.data + lastData.dataSize;
    } else {
        lastDataEnd = _linkeditStart;
    }
    if (lastDataEnd + shiftDelta > _linkeditEnd) {
        return false;
    }

    // Shift all data after `after`
    uint8_t *shiftStart =
        after ? after->data + after->dataSize : _linkeditStart;
    memmove(shiftStart + shiftDelta, shiftStart, lastDataEnd - shiftStart);

    // Update tracking data
    for (auto &trackedData : trackingData) {
        if (trackedData.data >= shiftStart) {
            *(uint32_t *)trackedData.offset += shiftDelta;
            trackedData.data += shiftDelta;
        }
    }

    // zero out pointer align padding and set data
    memset(shiftStart + shiftDelta - 8, 0, 8);
    memcpy(shiftStart, data.data, data.dataSize);

    // update data and add to tracking
    data.data = shiftStart;
    data.dataSize = shiftDelta; // include padding
    trackData(data);
    return true;
}

template <class P> void LinkeditTracker<P>::trackData(LinkeditData data) {
    auto it =
        std::lower_bound(trackingData.begin(), trackingData.end(), data,
                         [](const LinkeditData &a, const LinkeditData &b) {
                             return a.data < b.data;
                         });
    trackingData.insert(it, data);
}

template class LinkeditTracker<Utils::Pointer32>;
template class LinkeditTracker<Utils::Pointer64>;

class StringPool {
  public:
    StringPool();

    /// Add a string to the string pool.
    ///
    /// @param string The string to add.
    /// @returns The string index.
    uint32_t addString(const char *string);

    /// Write strings
    ///
    /// @param dest Buffer to write to.
    /// @returns The size of the strings written.
    uint32_t writeStrings(uint8_t *dest);

  private:
    std::map<std::string_view, uint32_t> _pool;
    uint32_t _stringsLength = 0;
};

StringPool::StringPool() {
    // first string is \x00 historically
    addString("\x00");
}

uint32_t StringPool::addString(const char *string) {
    std::string_view strView(string);
    if (_pool.contains(strView)) {
        return _pool[strView];
    }

    uint32_t index = _stringsLength;

    _pool[strView] = index;
    _stringsLength += (uint32_t)strView.length() + 1;
    return index;
}

uint32_t StringPool::writeStrings(uint8_t *dest) {
    std::vector<std::pair<uint32_t, std::string_view>> strings;
    for (auto const &pair : _pool) {
        strings.emplace_back(pair.second, pair.first);
    }

    std::sort(strings.begin(), strings.end());

    for (auto &[offset, string] : strings) {
        memcpy(dest + offset, string.data(), string.length());
    }

    auto &[lastStringOff, lastString] = *strings.rbegin();
    return lastStringOff + (uint32_t)lastString.length() + 1;
}

template <class P> class LinkeditOptimizer {
  public:
    LinkeditOptimizer(Utils::ExtractionContext<P> &eCtx);

    void copyBindingInfo(uint8_t *newLinkedit, uint32_t &offset);
    void copyWeakBindingInfo(uint8_t *newLinkedit, uint32_t &offset);
    void copyLazyBindingInfo(uint8_t *newLinkedit, uint32_t &offset);
    void copyExportInfo(uint8_t *newLinkedit, uint32_t &offset);

    void startSymbolEntries(uint8_t *newLinkedit, uint32_t &offset);
    void searchRedactedSymbol(uint8_t *newLinkedit, uint32_t &offset);
    void copyLocalSymbols(uint8_t *newLinkedit, uint32_t &offset);
    void copyExportedSymbols(uint8_t *newLinkedit, uint32_t &offset);
    void copyImportedSymbols(uint8_t *newLinkedit, uint32_t &offset);
    void endSymbolEntries(uint8_t *newLinkedit, uint32_t &offset);

    void copyFunctionStarts(uint8_t *newLinkedit, uint32_t &offset);
    void copyDataInCode(uint8_t *newLinkedit, uint32_t &offset);
    void copyIndirectSymbolTable(uint8_t *newLinkedit, uint32_t &offset);
    void copyStringPool(uint8_t *newLinkedit, uint32_t &offset);

    void updateLoadCommands(uint32_t &offset);

  private:
    /// Finds the start of the local symbols and how many there are.
    std::tuple<Macho::Loader::nlist<P> *, Macho::Loader::nlist<P> *>
    _findLocalSymbolEntries(const Dyld::Context *symbolsCache,
                            dyld_cache_local_symbols_info *symbolsInfo);

    uint32_t _copyPublicLocalSymbols(uint8_t *newLinkedit, uint32_t &offset);
    uint32_t _copyRedactedLocalSymbols(uint8_t *newLinkedit, uint32_t &offset);

    Utils::ExtractionContext<P> &_eCtx;
    Dyld::Context &_dCtx;
    Macho::Context<false, P> &_mCtx;
    ActivityLogger &_activity;
    std::shared_ptr<spdlog::logger> _logger;
    LinkeditTracker<P> &_linkeditTracker;

    StringPool _stringsPool;
    uint32_t _symbolsCount = 0;

    uint8_t *_linkeditFile;
    uint32_t _linkeditOffset;
    uint8_t *_linkeditStart;
    Macho::Loader::dyld_info_command *_dyldInfo;
    Macho::Loader::symtab_command *_symTab;
    Macho::Loader::dysymtab_command *_dySymTab;
    Macho::Loader::linkedit_data_command *_exportTrieCmd;

    uint32_t _newSymbolEntriesStart = 0;
    uint32_t _redactedSymbolsCount = 0;
    std::map<uint32_t, uint32_t> _newSymbolIndicies;
};

template <class P>
LinkeditOptimizer<P>::LinkeditOptimizer(Utils::ExtractionContext<P> &eCtx)
    : _eCtx(eCtx), _dCtx(eCtx.dCtx), _mCtx(eCtx.mCtx),
      _activity(*eCtx.activity), _logger(eCtx.logger),
      _linkeditTracker(*eCtx.linkeditTracker) {
    auto &mCtx = eCtx.mCtx;

    auto [offset, file] =
        mCtx.convertAddr(mCtx.getSegment("__LINKEDIT")->command->vmaddr);
    _linkeditFile = file;
    _linkeditOffset = (uint32_t)offset;
    _linkeditStart = file + offset;

    _dyldInfo = mCtx.getLoadCommand<false, Macho::Loader::dyld_info_command>();
    _symTab = mCtx.getLoadCommand<false, Macho::Loader::symtab_command>();
    _dySymTab = mCtx.getLoadCommand<false, Macho::Loader::dysymtab_command>();
    _exportTrieCmd =
        mCtx.getLoadCommand<false, Macho::Loader::linkedit_data_command>(
            {LC_DYLD_EXPORTS_TRIE});
}

template <class P>
void LinkeditOptimizer<P>::copyBindingInfo(uint8_t *newLinkedit,
                                           uint32_t &offset) {
    if (!_dyldInfo) {
        return;
    }

    if (auto size = _dyldInfo->bind_size) {
        _activity.update(std::nullopt, "Copying binding info");
        memcpy(newLinkedit + offset, _linkeditFile + _dyldInfo->bind_off, size);

        Utils::alignR(size, 8);
        _linkeditTracker.trackData(LinkeditData(
            (uint8_t *)_dyldInfo +
                offsetof(Macho::Loader::dyld_info_command, bind_off),
            _linkeditStart + offset, size));
        _dyldInfo->bind_off = _linkeditOffset + offset;

        offset += size;
    }
    _activity.update();
}

template <class P>
void LinkeditOptimizer<P>::copyWeakBindingInfo(uint8_t *newLinkedit,
                                               uint32_t &offset) {
    if (!_dyldInfo) {
        return;
    }

    if (auto size = _dyldInfo->weak_bind_size) {
        _activity.update(std::nullopt, "Copying weak binding info");
        memcpy(newLinkedit + offset, _linkeditFile + _dyldInfo->weak_bind_off,
               size);

        Utils::alignR(size, 8);
        _linkeditTracker.trackData(LinkeditData(
            (uint8_t *)_dyldInfo +
                offsetof(Macho::Loader::dyld_info_command, weak_bind_off),
            _linkeditStart + offset, size));
        _dyldInfo->weak_bind_off = _linkeditOffset + offset;

        offset += size;
    }

    _activity.update();
}

template <class P>
void LinkeditOptimizer<P>::copyLazyBindingInfo(uint8_t *newLinkedit,
                                               uint32_t &offset) {
    if (!_dyldInfo) {
        return;
    }

    if (auto size = _dyldInfo->lazy_bind_size) {
        _activity.update(std::nullopt, "Copying lazy binding info");
        memcpy(newLinkedit + offset, _linkeditFile + _dyldInfo->lazy_bind_off,
               size);

        Utils::alignR(size, 8);
        _linkeditTracker.trackData(LinkeditData(
            (uint8_t *)_dyldInfo +
                offsetof(Macho::Loader::dyld_info_command, lazy_bind_off),
            _linkeditStart + offset, size));
        _dyldInfo->lazy_bind_off = _linkeditOffset + offset;

        offset += size;
    }

    _activity.update();
}

template <class P>
void LinkeditOptimizer<P>::copyExportInfo(uint8_t *newLinkedit,
                                          uint32_t &offset) {
    if (!_exportTrieCmd && !_dyldInfo) {
        return;
    }

    uint8_t *data;
    uint32_t dataSize;
    uint8_t *dataFieldOff;
    if (_exportTrieCmd) {
        data = _linkeditFile + _exportTrieCmd->dataoff;
        dataSize = _exportTrieCmd->datasize;
        dataFieldOff = (uint8_t *)_exportTrieCmd +
                       offsetof(Macho::Loader::linkedit_data_command, dataoff);
    } else {
        data = _linkeditFile + _dyldInfo->export_off;
        dataSize = _dyldInfo->export_size;
        dataFieldOff = (uint8_t *)_dyldInfo +
                       offsetof(Macho::Loader::dyld_info_command, export_off);
    }

    if (dataSize) {
        _activity.update(std::nullopt, "Copying export info");
        memcpy(newLinkedit + offset, data, dataSize);

        Utils::alignR(dataSize, 8);
        _linkeditTracker.trackData(
            LinkeditData(dataFieldOff, _linkeditStart + offset, dataSize));
        *(uint32_t *)dataFieldOff = _linkeditStart + offset;

        offset += dataSize;
    }

    _activity.update();
}

template <class P>
void LinkeditOptimizer<P>::startSymbolEntries(uint8_t *newLinkedit,
                                              uint32_t &offset) {
    _newSymbolEntriesStart = offset;
}

template <class P>
void LinkeditOptimizer<P>::searchRedactedSymbol(uint8_t *newLinkedit,
                                                uint32_t &offset) {
    _activity.update(std::nullopt, "Searching for redacted symbols");

    uint8_t *indirectSymStart = _linkeditFile + _dySymTab->indirectsymoff;
    for (std::size_t i = 0; i < _dySymTab->nindirectsyms; i++) {
        auto symbolIndex =
            (uint32_t *)(indirectSymStart + i * sizeof(uint32_t));
        if (*symbolIndex == 0) {
            _redactedSymbolsCount++;
        }
    }

    if (_redactedSymbolsCount) {
        auto strIndex = _stringsPool.addString("<redacted>");
        _symbolsCount++;

        auto symbolEntry = (Macho::Loader::nlist<P> *)(newLinkedit + offset);
        symbolEntry->n_un.n_strx = strIndex;
        symbolEntry->n_type = 1;

        offset += sizeof(Macho::Loader::nlist<P>);
        _eCtx.hasRedactedIndirect = true;
    }
}

template <class P>
void LinkeditOptimizer<P>::copyLocalSymbols(uint8_t *newLinkedit,
                                            uint32_t &offset) {
    _activity.update(std::nullopt, "Copying local symbols");

    uint32_t newLocalSymbolsStartIndex = _symbolsCount;
    uint32_t newSymsCount = _copyPublicLocalSymbols(newLinkedit, offset);
    newSymsCount += _copyRedactedLocalSymbols(newLinkedit, offset);

    if (newSymsCount && _dySymTab) {
        _dySymTab->ilocalsym = newLocalSymbolsStartIndex;
        _dySymTab->nlocalsym = newSymsCount;
    }
}

template <class P>
void LinkeditOptimizer<P>::copyExportedSymbols(uint8_t *newLinkedit,
                                               uint32_t &offset) {
    _activity.update(std::nullopt, "Copying exported symbols");

    if (!_dySymTab) {
        SPDLOG_LOGGER_WARN(_logger, "Unable to copy exported symbols");
        return;
    }

    uint32_t newExportedSymbolsStartIndex = _symbolsCount;
    uint32_t newExportedSymbolsCount = 0;
    auto syms = (Macho::Loader::nlist<P> *)(_linkeditFile + _symTab->symoff);
    uint32_t symsStart = _dySymTab->iextdefsym;
    uint32_t symsEnd = symsStart + _dySymTab->nextdefsym;
    auto stringsStart = _linkeditFile + _symTab->stroff;
    auto newEntriesHead = (Macho::Loader::nlist<P> *)(newLinkedit + offset);

    for (auto symIndex = symsStart; symIndex < symsEnd; symIndex++) {
        _activity.update();
        auto symEntry = syms + symIndex;
        const char *string = (const char *)stringsStart + symEntry->n_un.n_strx;

        Macho::Loader::nlist<P> *newEntry = newEntriesHead;
        memcpy(newEntry, syms + symIndex, sizeof(Macho::Loader::nlist<P>));
        newEntry->n_un.n_strx = _stringsPool.addString(string);
        newEntriesHead++;

        _newSymbolIndicies[symIndex] = _symbolsCount;

        newExportedSymbolsCount++;
        _symbolsCount++;
    }

    if (newExportedSymbolsCount) {
        _dySymTab->iextdefsym = newExportedSymbolsStartIndex;
        _dySymTab->nextdefsym = newExportedSymbolsCount;
    }
    offset += sizeof(Macho::Loader::nlist<P>) * newExportedSymbolsCount;
}

template <class P>
void LinkeditOptimizer<P>::copyImportedSymbols(uint8_t *newLinkedit,
                                               uint32_t &offset) {
    _activity.update(std::nullopt, "Copying imported symbols");

    if (!_dySymTab) {
        SPDLOG_LOGGER_WARN(_logger, "Unable to copy imported symbols");
        return;
    }

    uint32_t newImportedSymbolsStartIndex = _symbolsCount;
    uint32_t newImportedSymbolsCount = 0;
    auto syms = (Macho::Loader::nlist<P> *)(_linkeditFile + _symTab->symoff);
    uint32_t symsStart = _dySymTab->iundefsym;
    uint32_t symsEnd = symsStart + _dySymTab->nundefsym;
    auto stringsStart = _linkeditFile + _symTab->stroff;
    auto newEntriesHead = (Macho::Loader::nlist<P> *)(newLinkedit + offset);

    for (auto symIndex = symsStart; symIndex < symsEnd; symIndex++) {
        _activity.update();
        auto symEntry = syms + symIndex;
        const char *string = (const char *)stringsStart + symEntry->n_un.n_strx;

        Macho::Loader::nlist<P> *newEntry = newEntriesHead;
        memcpy(newEntry, syms + symIndex, sizeof(Macho::Loader::nlist<P>));
        newEntry->n_un.n_strx = _stringsPool.addString(string);
        newEntriesHead++;

        _newSymbolIndicies[symIndex] = _symbolsCount;

        newImportedSymbolsCount++;
        _symbolsCount++;
    }

    if (newImportedSymbolsCount) {
        _dySymTab->iundefsym = newImportedSymbolsStartIndex;
        _dySymTab->nundefsym = newImportedSymbolsCount;
    }
    offset += sizeof(Macho::Loader::nlist<P>) * newImportedSymbolsCount;
}

template <class P>
void LinkeditOptimizer<P>::endSymbolEntries(uint8_t *newLinkedit,
                                            uint32_t &offset) {
    if (!_symTab) {
        return;
    }

    // Add room for redacted symbol entries that can be fixed later.
    offset += sizeof(Macho::Loader::nlist<P>) * _redactedSymbolsCount;

    auto symEntrySize = (uint32_t)(offset - _newSymbolEntriesStart);
    Utils::alignR(symEntrySize, 8);

    _linkeditTracker.trackData(LinkeditData(
        (uint8_t *)_symTab + offsetof(Macho::Loader::symtab_command, symoff),
        _linkeditStart + _newSymbolEntriesStart, symEntrySize));
    _symTab->symoff = _linkeditOffset + _newSymbolEntriesStart;
    _symTab->nsyms = _symbolsCount;
}

template <class P>
void LinkeditOptimizer<P>::copyFunctionStarts(uint8_t *newLinkedit,
                                              uint32_t &offset) {
    auto functionStarts =
        _mCtx.getLoadCommand<false, Macho::Loader::linkedit_data_command>(
            {LC_FUNCTION_STARTS});
    if (!functionStarts) {
        return;
    }

    if (auto size = functionStarts->datasize) {
        _activity.update(std::nullopt, "Copying function starts");
        memcpy(newLinkedit + offset, _linkeditFile + functionStarts->dataoff,
               size);

        Utils::alignR(size, 8);
        _linkeditTracker.trackData(LinkeditData(
            (uint8_t *)functionStarts +
                offsetof(Macho::Loader::linkedit_data_command, dataoff),
            _linkeditStart + offset, size));
        functionStarts->dataoff = _linkeditOffset + offset;

        offset += size;
    }

    _activity.update();
}

template <class P>
void LinkeditOptimizer<P>::copyDataInCode(uint8_t *newLinkedit,
                                          uint32_t &offset) {
    auto dataInCode =
        _mCtx.getLoadCommand<false, Macho::Loader::linkedit_data_command>(
            {LC_DATA_IN_CODE});
    if (!dataInCode) {
        return;
    }

    if (auto size = dataInCode->datasize) {
        _activity.update(std::nullopt, "Copying data in code");
        memcpy(newLinkedit + offset, _linkeditFile + dataInCode->dataoff, size);

        Utils::alignR(size, 8);
        _linkeditTracker.trackData(LinkeditData(
            (uint8_t *)dataInCode +
                offsetof(Macho::Loader::linkedit_data_command, dataoff),
            _linkeditStart + offset, size));
        dataInCode->dataoff = _linkeditOffset + offset;

        offset += size;
    }

    _activity.update();
}

template <class P>
void LinkeditOptimizer<P>::copyIndirectSymbolTable(uint8_t *newLinkedit,
                                                   uint32_t &offset) {
    if (!_dySymTab) {
        return;
    }

    _activity.update(std::nullopt, "Copying indirect symbol table");

    auto entries = (uint32_t *)(_linkeditFile + _dySymTab->indirectsymoff);
    auto newEntries = (uint32_t *)(newLinkedit + offset);
    for (uint32_t entryIndex = 0; entryIndex < _dySymTab->nindirectsyms;
         entryIndex++) {
        uint32_t *entry = entries + entryIndex;
        if (*entry == INDIRECT_SYMBOL_ABS || *entry == INDIRECT_SYMBOL_LOCAL ||
            *entry == 0) {
            // just copy entry
            *(newEntries + entryIndex) = *entry;
        }

        *(newEntries + entryIndex) = _newSymbolIndicies[*entry];
        _activity.update();
    }

    uint32_t size = _dySymTab->nindirectsyms * sizeof(uint32_t);
    Utils::alignR(size, 8);
    _linkeditTracker.trackData(LinkeditData(
        (uint8_t *)_dySymTab +
            offsetof(Macho::Loader::dysymtab_command, indirectsymoff),
        _linkeditStart + offset, size));
    _dySymTab->indirectsymoff = _linkeditOffset + offset;

    offset += size;
}

template <class P>
void LinkeditOptimizer<P>::copyStringPool(uint8_t *newLinkedit,
                                          uint32_t &offset) {
    _activity.update(std::nullopt, "Copying string pool");

    auto size = _stringsPool.writeStrings(newLinkedit + offset);
    _symTab->stroff = _linkeditOffset + offset;
    _symTab->strsize = size;

    Utils::alignR(size, 8);
    _linkeditTracker.trackData(LinkeditData(
        (uint8_t *)_symTab + offsetof(Macho::Loader::symtab_command, stroff),
        _linkeditStart + offset, size));

    _activity.update();
    offset += size;
}

template <class P>
void LinkeditOptimizer<P>::updateLoadCommands(uint32_t &offset) {
    // update segment
    auto linkeditSeg = _mCtx.getSegment("__LINKEDIT")->command;
    linkeditSeg->vmsize = offset;
    linkeditSeg->filesize = offset;
}

template <class P>
std::tuple<Macho::Loader::nlist<P> *, Macho::Loader::nlist<P> *>
LinkeditOptimizer<P>::_findLocalSymbolEntries(
    const Dyld::Context *symbolsCache,
    dyld_cache_local_symbols_info *symbolsInfo) {
    // Search local symbol entries for the macho context.
    auto searchEntries =
        [&]<class T>(decltype(T::dylibOffset) machoOffset) -> T * {
        T *entriesStart =
            (T *)((uint8_t *)symbolsInfo + symbolsInfo->entriesOffset);
        T *entriesEnd = entriesStart + symbolsInfo->entriesCount;
        for (auto entry = entriesStart; entry < entriesEnd; entry++) {
            if (entry->dylibOffset == machoOffset) {
                return entry;
            }
        }

        return nullptr;
    };

    uint8_t *nlistStart = nullptr;
    uint32_t nlistCount = 0;
    if (_dCtx.headerContainsMember(
            offsetof(dyld_cache_header, symbolFileUUID))) {
        // Newer caches, vm offset to mach header.
        uint64_t machoOffset = _mCtx.getSegment("__TEXT")->command->vmaddr -
                               _dCtx.header->sharedRegionStart;
        auto entry =
            searchEntries.operator()<dyld_cache_local_symbols_entry_64>(
                machoOffset);
        if (entry) {
            nlistStart =
                (uint8_t *)symbolsInfo + symbolsInfo->nlistOffset +
                sizeof(Macho::Loader::nlist<P>) * entry->nlistStartIndex;
            nlistCount = entry->nlistCount;
        }
    } else {
        // Older caches, file offset to mach header.
        uint64_t machoOffset =
            _mCtx.convertAddr(_mCtx.getSegment("__TEXT")->command->vmaddr)
                .first;
        auto entry = searchEntries.operator()<dyld_cache_local_symbols_entry>(
            (uint32_t)machoOffset);
        if (entry) {
            nlistStart =
                (uint8_t *)symbolsInfo + symbolsInfo->nlistOffset +
                sizeof(Macho::Loader::nlist<P>) * entry->nlistStartIndex;
            nlistCount = entry->nlistCount;
        }
    }

    if (!nlistStart) {
        SPDLOG_LOGGER_ERROR(_logger, "Unable to find local symbol entries.");
        return std::make_tuple(nullptr, nullptr);
    }

    return std::make_tuple((Macho::Loader::nlist<P> *)nlistStart,
                           (Macho::Loader::nlist<P> *)nlistStart + nlistCount);
}

template <class P>
uint32_t LinkeditOptimizer<P>::_copyPublicLocalSymbols(uint8_t *newLinkedit,
                                                       uint32_t &offset) {
    if (!_dySymTab || !_dySymTab->nlocalsym) {
        return 0;
    }

    uint32_t newLocalSymbolsCount = 0;
    auto strings = (const char *)_linkeditFile + _symTab->stroff;
    auto symsStart =
        (Macho::Loader::nlist<P> *)(_linkeditFile + _symTab->symoff) +
        _dySymTab->ilocalsym;
    auto symsEnd = symsStart + _dySymTab->nlocalsym;
    auto newEntriesHead = (Macho::Loader::nlist<P> *)(newLinkedit + offset);

    for (auto entry = symsStart; entry < symsEnd; entry++) {
        const char *string = strings + entry->n_un.n_strx;
        if (std::strcmp(string, "<redacted>") == 0) {
            continue;
        }

        Macho::Loader::nlist<P> *newEntry = newEntriesHead;
        memcpy(newEntry, entry, sizeof(Macho::Loader::nlist<P>));
        newEntry->n_un.n_strx = _stringsPool.addString(string);
        newEntriesHead++;

        newLocalSymbolsCount++;
        _symbolsCount++;
        _activity.update();
    }

    offset += sizeof(Macho::Loader::nlist<P>) * newLocalSymbolsCount;
    return newLocalSymbolsCount;
}

template <class P>
uint32_t LinkeditOptimizer<P>::_copyRedactedLocalSymbols(uint8_t *newLinkedit,
                                                         uint32_t &offset) {
    auto symbolsCache = _dCtx.getSymbolsCache();
    if (!symbolsCache || !symbolsCache->header->localSymbolsOffset) {
        return 0;
    }

    auto localSymsInfo =
        (dyld_cache_local_symbols_info
             *)(symbolsCache->file + symbolsCache->header->localSymbolsOffset);
    auto [symsStart, symsEnd] =
        _findLocalSymbolEntries(symbolsCache, localSymsInfo);
    if (!symsStart) {
        SPDLOG_LOGGER_ERROR(_logger, "Unable to copy redacted local symbols.");
    }

    uint32_t newLocalSymbolsCount = 0;
    auto newEntriesHead = (Macho::Loader::nlist<P> *)(newLinkedit + offset);
    auto stringsStart = (uint8_t *)localSymsInfo + localSymsInfo->stringsOffset;
    for (auto symEntry = symsStart; symEntry < symsEnd; symEntry++) {
        _activity.update();
        const char *string = (const char *)stringsStart + symEntry->n_un.n_strx;

        Macho::Loader::nlist<P> *newEntry = newEntriesHead;
        memcpy(newEntry, symEntry, sizeof(Macho::Loader::nlist<P>));
        newEntry->n_un.n_strx = _stringsPool.addString(string);
        newEntriesHead++;

        newLocalSymbolsCount++;
        _symbolsCount++;
    }

    offset += sizeof(Macho::Loader::nlist<P>) * newLocalSymbolsCount;
    return newLocalSymbolsCount;
}

/// Check all load commands for unknown load commands
template <class P> void checkLoadCommands(Utils::ExtractionContext<P> &eCtx) {
    for (auto lc : eCtx.mCtx.loadCommands) {
        switch (lc->cmd) {
        case LC_SEGMENT:    // segment_command
        case LC_SEGMENT_64: // segment_command_64
        case LC_IDFVMLIB:   // fvmlib_command
        case LC_LOADFVMLIB:
        case LC_ID_DYLIB: // dylib_command
        case LC_LOAD_DYLIB:
        case LC_LOAD_WEAK_DYLIB:
        case LC_REEXPORT_DYLIB:
        case LC_LOAD_UPWARD_DYLIB:
        case LC_LAZY_LOAD_DYLIB:
        case LC_SUB_FRAMEWORK:  // sub_framework_command
        case LC_SUB_CLIENT:     // sub_client_command
        case LC_SUB_UMBRELLA:   // sub_umbrella_command
        case LC_SUB_LIBRARY:    // sub_library_command
        case LC_PREBOUND_DYLIB: // prebound_dylib_command
        case LC_ID_DYLINKER:    // dylinker_command
        case LC_LOAD_DYLINKER:
        case LC_DYLD_ENVIRONMENT:
        case LC_THREAD: // thread_command
        case LC_UNIXTHREAD:
        case LC_ROUTINES:           // routines_command
        case LC_ROUTINES_64:        // routines_command_64
        case LC_PREBIND_CKSUM:      // prebind_cksum_command
        case LC_UUID:               // uuid_command
        case LC_RPATH:              // rpath_command
        case LC_FILESET_ENTRY:      // fileset_entry_command
        case LC_ENCRYPTION_INFO:    // encryption_info_command
        case LC_ENCRYPTION_INFO_64: // encryption_info_command_64
        case LC_VERSION_MIN_MACOSX: // version_min_command
        case LC_VERSION_MIN_IPHONEOS:
        case LC_VERSION_MIN_WATCHOS:
        case LC_VERSION_MIN_TVOS:
        case LC_BUILD_VERSION:  // build_version_command
        case LC_LINKER_OPTION:  // linker_option_command
        case LC_IDENT:          // ident_command
        case LC_FVMFILE:        // fvmfile_command
        case LC_MAIN:           // entry_point_command
        case LC_SOURCE_VERSION: // source_version_command
            /* Don't contain any data in the linkedit */
            break;

        case LC_SYMTAB:            // symtab_command
        case LC_DYSYMTAB:          // dysymtab_command
        case LC_DYLD_EXPORTS_TRIE: // linkedit_data_command
        case LC_FUNCTION_STARTS:
        case LC_DATA_IN_CODE:
        case LC_DYLD_INFO: // dyld_info_command
        case LC_DYLD_INFO_ONLY:
            // Contains linkedit data, is properly handled.
            break;

        case LC_TWOLEVEL_HINTS: // twolevel_hints_command
        case LC_CODE_SIGNATURE: // linkedit_data_command
        case LC_SEGMENT_SPLIT_INFO:
        case LC_DYLIB_CODE_SIGN_DRS:
        case LC_LINKER_OPTIMIZATION_HINT:
        case LC_DYLD_CHAINED_FIXUPS:
        case LC_SYMSEG: // symseg_command, deprecated
        case LC_NOTE:   // note_command
            // May contain linkedit data, not handled.
            SPDLOG_LOGGER_WARN(
                eCtx.logger,
                "Unhandled load command: {:#x}, may contain linkedit data.",
                lc->cmd);
            break;
        default:
            SPDLOG_LOGGER_WARN(
                eCtx.logger,
                "Unknown load command: {:#x}, may contain linkedit data.",
                lc->cmd);
            break;
        }
    }
}

template <class P>
void Converter::optimizeLinkedit(Utils::ExtractionContext<P> &eCtx) {
    eCtx.activity->update("Linkedit Optimizer", "Optimizing Linkedit");
    checkLoadCommands(eCtx);
    eCtx.linkeditTracker = new LinkeditTracker<P>(eCtx.mCtx);

    auto linkeditSeg = eCtx.mCtx.getSegment("__LINKEDIT");
    if (!linkeditSeg) {
        throw std::invalid_argument(
            "Mach-o file doesn't have __LINKEDIT segment.");
    }

    uint32_t offset = 0;
    uint8_t *newLinkedit = (uint8_t *)calloc(linkeditSeg->command->vmsize, 1);

    LinkeditOptimizer<P> optimizer = LinkeditOptimizer<P>(eCtx);
    optimizer.copyBindingInfo(newLinkedit, offset);
    optimizer.copyWeakBindingInfo(newLinkedit, offset);
    optimizer.copyLazyBindingInfo(newLinkedit, offset);

    optimizer.startSymbolEntries(newLinkedit, offset);
    optimizer.searchRedactedSymbol(newLinkedit, offset);
    optimizer.copyLocalSymbols(newLinkedit, offset);
    optimizer.copyExportedSymbols(newLinkedit, offset);
    optimizer.copyImportedSymbols(newLinkedit, offset);
    optimizer.endSymbolEntries(newLinkedit, offset);

    optimizer.copyFunctionStarts(newLinkedit, offset);
    optimizer.copyDataInCode(newLinkedit, offset);
    optimizer.copyIndirectSymbolTable(newLinkedit, offset);
    optimizer.copyStringPool(newLinkedit, offset);

    // Copy new linkedit
    auto oldLinkedit = eCtx.mCtx.convertAddrP(linkeditSeg->command->vmaddr);
    memcpy(oldLinkedit, newLinkedit, offset);
    optimizer.updateLoadCommands(offset);

    free(newLinkedit);
    return;
}

template void Converter::optimizeLinkedit<Utils::Pointer32>(
    Utils::ExtractionContext<Utils::Pointer32> &eCtx);
template void Converter::optimizeLinkedit<Utils::Pointer64>(
    Utils::ExtractionContext<Utils::Pointer64> &eCtx);