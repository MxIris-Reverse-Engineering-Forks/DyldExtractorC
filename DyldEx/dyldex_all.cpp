#include <filesystem>
#include <fstream>
#include <iostream>

#include <argparse/argparse.hpp>
#include <spdlog/spdlog.h>

#include <Converter/Linkedit/Linkedit.h>
#include <Converter/Objc/Objc.h>
#include <Converter/OffsetOptimizer.h>
#include <Converter/Slide.h>
#include <Converter/Stubs/Stubs.h>
#include <Dyld/DyldContext.h>
#include <Macho/MachoContext.h>
#include <Provider/Accelerator.h>
#include <Provider/Validator.h>
#include <Utils/ExtractionContext.h>

#include "config.h"

namespace fs = std::filesystem;
using namespace DyldExtractor;

#pragma region Arguments
struct ProgramArguments {
  fs::path cache_path;
  std::optional<fs::path> outputDir;
  bool verbose;
  bool disableOutput;
  bool onlyValidate;
  bool imbedVersion;

  union {
    uint32_t raw;
    struct {
      uint32_t processSlideInfo : 1, optimizeLinkedit : 1, fixStubs : 1,
          fixObjc : 1, generateMetadata : 1, unused : 27;
    };
  } modulesDisabled;
};

ProgramArguments parseArgs(int argc, char *argv[]) {
  argparse::ArgumentParser program("dyldex_all", DYLDEXTRACTORC_VERSION);

  program.add_argument("cache_path")
      .help("The path to the shared cache. If there are subcaches, give the "
            "main one (typically without the file extension).");

  program.add_argument("-o", "--output-dir")
      .help("The output directory for the extracted images. Required for "
            "extraction");

  program.add_argument("-v", "--verbose")
      .help("Enables debug logging messages.")
      .default_value(false)
      .implicit_value(true);

  program.add_argument("-d", "--disable-output")
      .help("Disables writing output. Useful for development.")
      .default_value(false)
      .implicit_value(true);

  program.add_argument("--only-validate")
      .help("Only validate images.")
      .default_value(false)
      .implicit_value(true);

  program.add_argument("-s", "--skip-modules")
      .help("Skip certain modules. Most modules depend on each other, so use "
            "with caution. Useful for development. 1=processSlideInfo, "
            "2=optimizeLinkedit, 4=fixStubs, 8=fixObjc, 16=generateMetadata")
      .scan<'d', int>()
      .default_value(0);

  program.add_argument("--imbed-version")
      .help("Imbed this tool's version number into the mach_header_64's "
            "reserved field. Only supports 64 bit images.")
      .default_value(false)
      .implicit_value(true);

  ProgramArguments args;
  try {
    program.parse_args(argc, argv);

    args.cache_path = fs::path(program.get<std::string>("cache_path"));
    args.outputDir = program.present<std::string>("--output-dir");
    args.verbose = program.get<bool>("--verbose");
    args.disableOutput = program.get<bool>("--disable-output");
    args.onlyValidate = program.get<bool>("--only-validate");
    args.modulesDisabled.raw = program.get<int>("--skip-modules");
    args.imbedVersion = program.get<bool>("--imbed-version");

  } catch (const std::runtime_error &err) {
    std::cerr << "Argument parsing error: " << err.what() << std::endl;
    std::exit(1);
  }

  if (!args.disableOutput && !args.outputDir) {
    std::cerr << "Output directory is required for extraction" << std::endl;
    std::exit(1);
  }

  return args;
}
#pragma endregion Arguments

template <class A>
void runImage(Dyld::Context &dCtx,
              Provider::Accelerator<typename A::P> &accelerator,
              const dyld_cache_image_info *imageInfo,
              const std::string imagePath, const std::string imageName,
              const ProgramArguments &args, std::ostream &logStream) {

  // validate
  auto mCtx = dCtx.createMachoCtx<false, typename A::P>(imageInfo);
  try {
    Provider::Validator<typename A::P>(mCtx).validate();
  } catch (const std::exception &e) {
    std::cout << std::format("Validation Error: {}", e.what());
    return;
  }

  if (args.onlyValidate) {
    return;
  }

  // Setup context
  Provider::ActivityLogger activity("DyldEx_" + imageName, logStream, false);
  auto logger = activity.getLogger();
  logger->set_pattern("[%-8l %s:%#] %v");
  if (args.verbose) {
    logger->set_level(spdlog::level::trace);
  } else {
    logger->set_level(spdlog::level::info);
  }

  Utils::ExtractionContext<A> eCtx(dCtx, mCtx, accelerator, activity);

  if (!args.modulesDisabled.processSlideInfo) {
    Converter::processSlideInfo(eCtx);
  }
  if (!args.modulesDisabled.optimizeLinkedit) {
    Converter::optimizeLinkedit(eCtx);
  }
  if (!args.modulesDisabled.fixStubs) {
    Converter::fixStubs(eCtx);
  }
  if (!args.modulesDisabled.fixObjc) {
    Converter::fixObjc(eCtx);
  }
  if (!args.modulesDisabled.generateMetadata) {
    Converter::generateMetadata(eCtx);
  }
  if (args.imbedVersion) {
    if constexpr (!std::is_same_v<typename A::P, Utils::Arch::Pointer64>) {
      SPDLOG_LOGGER_ERROR(
          logger, "Unable to imbed version info in a non 64 bit image.");
    } else {
      mCtx.header->reserved = DYLDEXTRACTORC_VERSION_DATA;
    }
  }

  if (!args.disableOutput) {
    auto writeProcedures = Converter::optimizeOffsets(eCtx);

    auto outputPath = *args.outputDir / imagePath.substr(1); // remove leading /
    fs::create_directories(outputPath.parent_path());
    std::ofstream outFile(outputPath, std::ios_base::binary);
    if (!outFile.good()) {
      SPDLOG_LOGGER_ERROR(logger, "Unable to open output file.");
      return;
    }

    for (auto procedure : writeProcedures) {
      outFile.seekp(procedure.writeOffset);
      outFile.write((const char *)procedure.source, procedure.size);
    }
    outFile.close();
  }
}

template <class A>
void runAllImages(Dyld::Context &dCtx, ProgramArguments &args) {
  Provider::ActivityLogger activity("DyldEx_All", std::cout, true);
  auto logger = activity.getLogger();
  logger->set_pattern("[%T:%e %-8l %s:%#] %v");
  if (args.verbose) {
    logger->set_level(spdlog::level::trace);
  } else {
    logger->set_level(spdlog::level::info);
  }
  activity.update("DyldEx All", "Starting up");
  int imagesProcessed = 0;
  std::ostringstream summaryStream;

  Provider::Accelerator<typename A::P> accelerator;

  const int numberOfImages = (int)dCtx.images.size();
  for (int i = 0; i < numberOfImages; i++) {
    const auto imageInfo = dCtx.images[i];
    std::string imagePath((char *)(dCtx.file + imageInfo->pathFileOffset));
    std::string imageName = imagePath.substr(imagePath.rfind("/") + 1);

    imagesProcessed++;
    activity.update(std::nullopt, fmt::format("[{:4}/{}] {}", imagesProcessed,
                                              numberOfImages, imageName));

    std::ostringstream loggerStream;
    runImage<A>(dCtx, accelerator, imageInfo, imagePath, imageName, args,
                loggerStream);

    // update summary and UI.
    auto logs = loggerStream.str();
    activity.getLoggerStream()
        << fmt::format("processed {}", imageName) << std::endl
        << logs << std::endl;
    if (logs.length()) {
      summaryStream << "* " << imageName << std::endl << logs << std::endl;
    }
  }

  activity.update(std::nullopt, "Done");
  activity.stopActivity();
  activity.getLoggerStream()
      << std::endl
      << "==== Summary ====" << std::endl
      << summaryStream.str() << "=================" << std::endl;
}

int main(int argc, char *argv[]) {
//  ProgramArguments args = parseArgs(argc, argv);
    ProgramArguments args = ProgramArguments();
    args.cache_path = "/System/Volumes/Preboot/Cryptexes/OS/System/Library/dyld/dyld_shared_cache_x86_64";
    args.verbose = true;
//    args.listImages = true;
    
    args.outputDir = "/Users/JH/Desktop";
  try {
    Dyld::Context dCtx(args.cache_path);

    // use dyld's magic to select arch
    if (strcmp(dCtx.header->magic, "dyld_v1  x86_64") == 0)
      runAllImages<Utils::Arch::x86_64>(dCtx, args);
    else if (strcmp(dCtx.header->magic, "dyld_v1 x86_64h") == 0)
      runAllImages<Utils::Arch::x86_64>(dCtx, args);
    else if (strcmp(dCtx.header->magic, "dyld_v1   armv7") == 0)
      runAllImages<Utils::Arch::arm>(dCtx, args);
    else if (strncmp(dCtx.header->magic, "dyld_v1  armv7", 14) == 0)
      runAllImages<Utils::Arch::arm>(dCtx, args);
    else if (strcmp(dCtx.header->magic, "dyld_v1   arm64") == 0)
      runAllImages<Utils::Arch::arm64>(dCtx, args);
    else if (strcmp(dCtx.header->magic, "dyld_v1  arm64e") == 0)
      runAllImages<Utils::Arch::arm64>(dCtx, args);
    else if (strcmp(dCtx.header->magic, "dyld_v1arm64_32") == 0)
      runAllImages<Utils::Arch::arm64_32>(dCtx, args);
    else if (strcmp(dCtx.header->magic, "dyld_v1    i386") == 0 ||
             strcmp(dCtx.header->magic, "dyld_v1   armv5") == 0 ||
             strcmp(dCtx.header->magic, "dyld_v1   armv6") == 0) {
      std::cerr << "Unsupported Architecture type.";
      return 1;
    } else {
      std::cerr << "Unrecognized dyld shared cache magic.\n";
      return 1;
    }

  } catch (const std::exception &e) {
    std::cerr << "An error has occurred: " << e.what() << std::endl;
    return 1;
  }
  return 0;
}
