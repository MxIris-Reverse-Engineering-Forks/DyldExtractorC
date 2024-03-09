#include "Validator.h"
#include <Utils/Architectures.h>

using namespace DyldExtractor;
using namespace Provider;

template <class P>
Validator<P>::Validator(const Macho::Context<false, P> &mCtx) : mCtx(&mCtx) {}

template <class P> void Validator<P>::validate() {
    if (!mCtx->getSegment(SEG_LINKEDIT)) {
        throw std::runtime_error("Missing Linkedit segment.");
    }
    
    if (!mCtx->getSegment(SEG_TEXT)) {
        throw std::runtime_error("Missing Text segment.");
    }
    
    if (!mCtx->getSection(SEG_TEXT, SECT_TEXT).second) {
        throw std::runtime_error("Missing text section.");
    }
    
    if (!mCtx->template getFirstLC<Macho::Loader::symtab_command>()) {
        throw std::runtime_error("Missing symtab command.");
    }
    
    if (!mCtx->template getFirstLC<Macho::Loader::dysymtab_command>()) {
        throw std::runtime_error("Missing dysymtab command.");
    }
    
    if (memcmp(mCtx->segments.back().command->segname, SEG_LINKEDIT,
               sizeof(SEG_LINKEDIT)) != 0) {
        throw std::runtime_error(
                             "Linkedit segment is not the last segment load command.");
    }
    
    {
        // Linkedit highest addr
        PtrT maxSegAddr = 0;
        PtrT leAddr = 0;
        for (const auto &seg : mCtx->segments) {
            if (memcmp(seg.command->segname, SEG_LINKEDIT, sizeof(SEG_LINKEDIT)) ==
                0) {
                leAddr = seg.command->vmaddr;
            } else {
                if (seg.command->vmaddr > maxSegAddr) {
                    maxSegAddr = seg.command->vmaddr;
                }
            }
        }
        
        if (maxSegAddr > leAddr) {
            throw std::runtime_error(
                                 "Linkedit segment does not have the highest address.");
        }
        
        if (leAddr % 0x4000) {
            throw std::runtime_error(
                                 "Linkedit segment is not address aligned to 0x4000.");
        }
    }
    
    if (!mCtx->template getFirstLC<Macho::Loader::linkedit_data_command>(
                                                                {LC_FUNCTION_STARTS})) {
                                                                    throw std::runtime_error("Missing function starts command.");
                                                                }
}

template class DyldExtractor::Provider::Validator<Utils::Arch::Pointer32>;
template class DyldExtractor::Provider::Validator<Utils::Arch::Pointer64>;
