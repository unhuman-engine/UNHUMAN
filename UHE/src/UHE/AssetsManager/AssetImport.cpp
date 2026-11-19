#include "uhepch.h"
#include "AssetImport.h"

#include <system_error>

namespace UHE
{
    namespace
    {
        void SetError(std::string* error, const std::string& message)
        {
            if (error != nullptr)
                *error = message;
        }

        // wall.png -> wall_1.png -> wall_2.png ... (first free name)
        std::filesystem::path UniqueDestination(const std::filesystem::path& folder,
                                                const std::filesystem::path& fileName)
        {
            std::error_code ec;
            std::filesystem::path candidate = folder / fileName;
            if (!std::filesystem::exists(candidate, ec))
                return candidate;

            const std::string stem = fileName.stem().string();
            const std::string extension = fileName.extension().string();
            for (int index = 1; index < 1000; ++index)
            {
                candidate = folder / (stem + "_" + std::to_string(index) + extension);
                if (!std::filesystem::exists(candidate, ec))
                    return candidate;
            }
            return {};
        }
    } // namespace

    std::filesystem::path ImportAssetFile(const std::filesystem::path& sourceFile,
                                          const std::filesystem::path& destinationFolder, std::string* error)
    {
        if (sourceFile.empty())
        {
            SetError(error, "no source file");
            return {};
        }
        if (destinationFolder.empty())
        {
            SetError(error, "no destination folder");
            return {};
        }

        std::error_code ec;
        if (!std::filesystem::is_regular_file(sourceFile, ec))
        {
            SetError(error, "'" + sourceFile.string() + "' is not a file");
            return {};
        }

        std::filesystem::create_directories(destinationFolder, ec);
        if (ec)
        {
            SetError(error, "could not create '" + destinationFolder.string() + "': " + ec.message());
            return {};
        }

        // Importing a file that already lives in the destination folder is a
        // no-op: the Content Browser's own drag payloads and re-drops land here.
        const std::filesystem::path absoluteSource = std::filesystem::absolute(sourceFile, ec).lexically_normal();
        const std::filesystem::path absoluteFolder = std::filesystem::absolute(destinationFolder, ec).lexically_normal();
        if (!ec && absoluteSource.parent_path() == absoluteFolder)
            return absoluteSource;

        const std::filesystem::path destination = UniqueDestination(absoluteFolder, sourceFile.filename());
        if (destination.empty())
        {
            SetError(error, "could not find a free name for '" + sourceFile.filename().string() + "'");
            return {};
        }

        std::filesystem::copy_file(sourceFile, destination, std::filesystem::copy_options::none, ec);
        if (ec)
        {
            SetError(error, "could not copy to '" + destination.string() + "': " + ec.message());
            return {};
        }

        return destination;
    }

    std::string MakeAssetRelativePath(const std::filesystem::path& file, const std::filesystem::path& base)
    {
        if (file.empty() || base.empty())
            return file.generic_string();

        std::error_code ec;
        const std::filesystem::path relative = std::filesystem::relative(file, base, ec);
        if (ec || relative.empty() || relative.generic_string().rfind("..", 0) == 0)
            return file.generic_string();
        return relative.generic_string();
    }
} // namespace UHE
