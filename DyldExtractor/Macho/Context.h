#ifndef __MACHO_CONTEXT__
#define __MACHO_CONTEXT__

#include <boost/iostreams/device/mapped_file.hpp>
#include <filesystem>

#include "Loader.h"
#include <dyld/dyld_cache_format.h>
#include <mach-o/loader.h>

namespace Macho {

namespace bio = boost::iostreams;
namespace fs = std::filesystem;

// Conditional const
template <bool _Test, class _T> struct c_const { using T = _T; };
template <class _T> struct c_const<true, _T> { using T = const _T; };

struct MappingInfo {
    uint64_t address;
    uint64_t size;
    uint64_t fileOffset;

    MappingInfo() = default;
    MappingInfo(const dyld_cache_mapping_info *);
};

template <bool ro, class P> class SegmentContext {
    using _SegmentCommandT = c_const<ro, Loader::segment_command<P>>::T;

  public:
    using SectionT = c_const<ro, Loader::section<P>>::T;

    _SegmentCommandT *command;
    std::vector<SectionT *> sections;

    SegmentContext(_SegmentCommandT *segment);
};

/// A wrapper around a MachO file in the DSC.
/// The template boolean determines if it is read only.
template <bool ro, class P> class Context {
    using _FileT = c_const<ro, uint8_t>::T;
    using _LoadCommandT = c_const<ro, Loader::load_command>::T;

  public:
    using HeaderT = c_const<ro, Loader::mach_header<P>>::T;

    // The file containing the header
    _FileT *file;
    HeaderT *header;

    std::vector<_LoadCommandT *> loadCommands;
    std::vector<typename SegmentContext<ro, P>> segments;

    /// A wrapper around a MachO file.
    ///
    /// The access permissions is based on the main file provided. Calling this
    /// directly also implies that the context does not manage the file maps.
    ///
    /// @param fileOffset The file offset to the mach header.
    /// @param mainFile The memory map that contains the header.
    /// @param mainMapping The mapping info for mainFile
    /// @param subFiles A vector of tuples of all other memory maps and their
    ///   mapping info.
    Context(uint64_t fileOffset, bio::mapped_file mainFile,
            std::vector<MappingInfo> mainMappings,
            std::vector<std::tuple<bio::mapped_file, std::vector<MappingInfo>>>
                subFiles);
    /// A writable wrapper around a MachO file.
    ///
    /// The files will be opened with private (copy on write) access.
    ///
    /// @param fileOffset The file offset to the mach header.
    /// @param mainPath The file path of the file that contains the header.
    /// @param mainMapping The mapping info for mainFile
    /// @param subFiles A vector of tuples of all other file paths and their
    ///   mapping info.
    Context(
        uint64_t fileOffset, fs::path mainPath,
        std::vector<MappingInfo> mainMappings,
        std::vector<std::tuple<fs::path, std::vector<MappingInfo>>> subFiles);

    /// Convert a vmaddr to it's file offset.
    /// If the offset could not be found, a tuple with 0 and
    /// nullptr will be returned.
    ///
    /// @param addr The virtual address to convert.
    /// @returns A pair of the file offset and its file.
    std::pair<uint64_t, _FileT *> convertAddr(uint64_t addr) const;

    /// Get load commands
    ///
    /// If multiple is false, the first match is returned.
    ///
    /// @tparam _m Whether to return multiple.
    /// @tparam _c Type of load command.
    /// @returns A vector of load command pointers or a single load command
    ///     pointer.
    template <bool _m, class _c, class _ct = c_const<ro, _c>::T>
    std::conditional<_m, std::vector<_ct *>, _ct *>::type
    getLoadCommand() const {
        auto ncmds = []<std::size_t _s>(const uint32_t(&)[_s]) constexpr {
            return _s;
        };

        if constexpr (_m) {
            return reinterpret_cast<std::vector<_ct *> &>(
                _getLoadCommands(_c::CMDS, ncmds(_c::CMDS)));
        } else {
            return (_ct *)_getLoadCommand(_c::CMDS, ncmds(_c::CMDS));
        }
    }

    /// Get load commands
    ///
    /// If multiple is false, the first match is returned.
    ///
    /// @tparam _m Whether to return multiple.
    /// @tparam _c Type of load command.
    /// @param cmds Overdrive the default set of command IDs associated with the
    ///     template command.
    /// @returns A vector of load command pointers or a single load command
    ///     pointer.
    template <bool _m, class _c, class _ct = c_const<ro, _c>::T, std::size_t _s>
    std::conditional<_m, std::vector<_ct *>, _ct *>::type
    getLoadCommand(const uint32_t (&cmds)[_s]) const {
        if constexpr (_m) {
            return reinterpret_cast<std::vector<_ct *> &>(
                _getLoadCommands(cmds, _s));
        } else {
            return (_ct *)_getLoadCommand(cmds, _s);
        }
    }

    /// Search for a segment
    ///
    /// @param segName The name of the segment.
    /// @returns The segment context. nullopt if not found.
    std::optional<SegmentContext<ro, P>> getSegment(const char *segName) const;

    /// Search for a section
    ///
    /// @param segName The name of the segment, or nullptr.
    /// @param sectName The name of the section.
    /// @returns The section structure, or nullptr.
    SegmentContext<ro, P>::SectionT *getSection(const char *segName,
                                                const char *sectName) const;

    ~Context();
    Context(const Context &other) = delete;
    Context(Context &&other);
    Context &operator=(const Context &other) = delete;
    Context &operator=(Context &&other);

  private:
    // Determines if the file need to be closed during destruction.
    bool _ownFiles = false;
    // Indicates if the files are open, false if _ownFiles is false.
    bool _filesOpen = false;

    // Contains all file maps
    std::vector<bio::mapped_file> _fileMaps;
    // Contains all files and mappings
    std::vector<std::tuple<_FileT *, std::vector<MappingInfo>>> _files;

    std::vector<_LoadCommandT *>
    _getLoadCommands(const uint32_t (&targetCmds)[], std::size_t ncmds) const;
    _LoadCommandT *_getLoadCommand(const uint32_t (&targetCmds)[],
                                   std::size_t ncmds) const;

    // Convenience method to open files with private access.
    static bio::mapped_file openFile(fs::path path);
    static std::vector<std::tuple<bio::mapped_file, std::vector<MappingInfo>>>
    openFiles(
        std::vector<std::tuple<fs::path, std::vector<MappingInfo>>> paths);
};

}; // namespace Macho

#endif // __MACHO_CONTEXT__