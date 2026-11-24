#include "uhepch.h"
#include "ShaderReflection.h"

#include <algorithm>
#include <unordered_map>
#include <unordered_set>

namespace UHE
{
namespace
{
// SPIR-V opcodes this reader understands. Anything else is skipped: a module is
// free to contain instructions that do not describe its interface.
enum : u16
{
    OpName = 5,
    OpMemberName = 6,
    OpEntryPoint = 15,
    OpExecutionMode = 16,
    OpTypeVoid = 19,
    OpTypeBool = 20,
    OpTypeInt = 21,
    OpTypeFloat = 22,
    OpTypeVector = 23,
    OpTypeMatrix = 24,
    OpTypeImage = 25,
    OpTypeSampler = 26,
    OpTypeSampledImage = 27,
    OpTypeArray = 28,
    OpTypeRuntimeArray = 29,
    OpTypeStruct = 30,
    OpTypePointer = 32,
    OpConstant = 43,
    OpVariable = 59,
    OpDecorate = 71,
    OpMemberDecorate = 72,
    OpExecutionModeId = 331,
};

enum : u32
{
    DecorationBlock = 2,
    DecorationBufferBlock = 3,
    DecorationRowMajor = 4,
    DecorationColMajor = 5,
    DecorationArrayStride = 6,
    DecorationMatrixStride = 7,
    DecorationBuiltIn = 11,
    DecorationLocation = 30,
    DecorationBinding = 33,
    DecorationDescriptorSet = 34,
    DecorationOffset = 35,
};

enum : u32
{
    ExecutionModeLocalSize = 17,
    ExecutionModeLocalSizeId = 38,
};

enum : u32
{
    StorageClassUniformConstant = 0,
    StorageClassInput = 1,
    StorageClassUniform = 2,
    StorageClassOutput = 3,
    StorageClassPushConstant = 9,
    StorageClassStorageBuffer = 12,
};

enum : u32
{
    ExecutionModelVertex = 0,
    ExecutionModelTessellationControl = 1,
    ExecutionModelTessellationEvaluation = 2,
    ExecutionModelGeometry = 3,
    ExecutionModelFragment = 4,
    ExecutionModelGLCompute = 5,
    ExecutionModelKernel = 6,
};

constexpr u32 kSpirvMagic = 0x07230203u;
constexpr u32 kNoDecoration = 0xFFFFFFFFu;

struct TypeInfo
{
    enum class Kind
    {
        Unknown,
        Void,
        Bool,
        Int,
        Float,
        Vector,
        Matrix,
        Image,
        Sampler,
        SampledImage,
        Array,
        RuntimeArray,
        Struct,
        Pointer,
    };

    Kind Which = Kind::Unknown;
    u32 ComponentType = 0;  // vector/matrix element, pointer pointee
    u32 Count = 0;          // vector components, matrix columns
    u32 Width = 0;          // int/float bit width
    bool Signed = false;    // int
    u32 StorageClass = 0;   // pointer
    u32 LengthId = 0;       // array
    u32 Sampled = 0;        // image: 0 unknown, 1 sampled, 2 storage
    std::vector<u32> Members;
};

struct Decoration
{
    u32 DescriptorSet = 0;
    u32 Binding = 0;
    u32 Location = 0;
    u32 ArrayStride = 0;
    u32 BuiltIn = kNoDecoration;
    bool HasDescriptorSet = false;
    bool HasBinding = false;
    bool HasLocation = false;
    bool HasArrayStride = false;
    bool Block = false;
    bool BufferBlock = false;
};

struct MemberDecoration
{
    u32 Offset = 0;
    u32 MatrixStride = 0;
    bool HasOffset = false;
    bool HasMatrixStride = false;
    bool RowMajor = false;
    bool ColMajor = false;
};

struct Variable
{
    u32 TypeId = 0;
    u32 StorageClass = 0;
};

struct EntryPoint
{
    u32 ExecutionModel = 0;
    std::string Name;
    std::vector<u32> Interface;
};

/// Reads a NUL-terminated literal string out of an instruction's words.
std::string ReadString(const u32* words, u32 wordCount, u32 firstWord)
{
    std::string text;
    for (u32 i = firstWord; i < wordCount; ++i)
    {
        const u32 packed = words[i];
        for (u32 byte = 0; byte < 4; ++byte)
        {
            const char c = static_cast<char>((packed >> (byte * 8)) & 0xFFu);
            if (c == '\0')
            {
                return text;
            }
            text.push_back(c);
        }
    }
    return text;
}

RHI::ShaderStage StageOfExecutionModel(u32 executionModel)
{
    switch (executionModel)
    {
        case ExecutionModelVertex:
            return RHI::ShaderStage::Vertex;
        case ExecutionModelFragment:
            return RHI::ShaderStage::Fragment;
        case ExecutionModelGLCompute:
        case ExecutionModelKernel:
            return RHI::ShaderStage::Compute;
        default:
            return RHI::ShaderStage::AllGraphics;
    }
}

RHI::ShaderDataType ScalarType(const TypeInfo& type)
{
    if (type.Which == TypeInfo::Kind::Int)
    {
        return type.Width > 32 ? RHI::ShaderDataType::None : RHI::ShaderDataType::Int;
    }
    if (type.Which == TypeInfo::Kind::Float)
    {
        return type.Width == 32 ? RHI::ShaderDataType::Float : RHI::ShaderDataType::None;
    }
    if (type.Which == TypeInfo::Kind::Bool)
    {
        return RHI::ShaderDataType::Bool;
    }
    return RHI::ShaderDataType::None;
}

struct Reader
{
    std::unordered_map<u32, TypeInfo> Types;
    std::unordered_map<u32, Decoration> Decorations;
    std::unordered_map<u32, std::unordered_map<u32, MemberDecoration>> MemberDecorations;
    std::unordered_map<u32, std::string> Names;
    std::unordered_map<u32, u32> Constants;
    std::unordered_map<u32, Variable> Variables;
    std::vector<EntryPoint> EntryPoints;
    std::vector<std::pair<u32, std::vector<u32>>> ExecutionModes; // entry point id -> words

    const TypeInfo* Type(u32 id) const
    {
        const auto it = Types.find(id);
        return it == Types.end() ? nullptr : &it->second;
    }

    const Decoration& DecorationOf(u32 id) const
    {
        static const Decoration empty;
        const auto it = Decorations.find(id);
        return it == Decorations.end() ? empty : it->second;
    }

    std::string NameOf(u32 id) const
    {
        const auto it = Names.find(id);
        return it == Names.end() ? std::string() : it->second;
    }

    /// \returns the pointee type of \p typeId when it is a pointer, else null.
    const TypeInfo* Pointee(u32 typeId) const
    {
        const TypeInfo* type = Type(typeId);
        if (type == nullptr || type->Which != TypeInfo::Kind::Pointer)
        {
            return nullptr;
        }
        return Type(type->ComponentType);
    }

    /// Constant value of \p id, or 0 when it is not a plain scalar constant.
    u32 ConstantValue(u32 id) const
    {
        const auto it = Constants.find(id);
        return it == Constants.end() ? 0u : it->second;
    }

    /// Size in bytes of a value of \p typeId, as std430 lays it out.
    ///
    /// \param matrixStride the enclosing member's MatrixStride, which decides how
    ///        much room a matrix takes; arrays and structs recurse with their own.
    u32 SizeOf(u32 typeId, u32 matrixStride, bool rowMajor, int depth) const
    {
        if (depth > 8)
        {
            return 0; // recursive type: not a layout the engine can use
        }

        const TypeInfo* type = Type(typeId);
        if (type == nullptr)
        {
            return 0;
        }

        switch (type->Which)
        {
            case TypeInfo::Kind::Bool:
                return 4;
            case TypeInfo::Kind::Int:
            case TypeInfo::Kind::Float:
                return type->Width / 8;
            case TypeInfo::Kind::Vector:
                return (type->Width > 0 ? type->Width / 8 : 4) * type->Count;
            case TypeInfo::Kind::Matrix:
            {
                const TypeInfo* column = Type(type->ComponentType);
                const u32 stride = matrixStride > 0 ? matrixStride : 16;
                // SPIR-V matrices are column-typed; a row-major member stores the
                // transpose, so the row count is what repeats.
                const u32 vectors = rowMajor && column != nullptr ? column->Count : type->Count;
                return stride * vectors;
            }
            case TypeInfo::Kind::Array:
            {
                const Decoration& decoration = DecorationOf(typeId);
                const u32 stride = decoration.HasArrayStride ? decoration.ArrayStride : 0;
                return stride * ConstantValue(type->LengthId);
            }
            case TypeInfo::Kind::RuntimeArray:
                return 0; // a runtime-sized array cannot live in a push constant block
            case TypeInfo::Kind::Struct:
            {
                u32 size = 0;
                for (u32 member = 0; member < type->Members.size(); ++member)
                {
                    const auto memberIt = MemberDecorations.find(typeId);
                    if (memberIt == MemberDecorations.end())
                    {
                        continue;
                    }
                    const auto decorationIt = memberIt->second.find(member);
                    if (decorationIt == memberIt->second.end() || !decorationIt->second.HasOffset)
                    {
                        continue;
                    }
                    const MemberDecoration& memberDecoration = decorationIt->second;
                    const u32 memberSize = SizeOf(type->Members[member], memberDecoration.MatrixStride,
                                                  memberDecoration.RowMajor, depth + 1);
                    size = std::max(size, memberDecoration.Offset + memberSize);
                }
                return size;
            }
            case TypeInfo::Kind::Pointer:
                return SizeOf(type->ComponentType, matrixStride, rowMajor, depth + 1);
            default:
                return 0;
        }
    }

    /// Maps a vertex attribute's type onto an RHI vertex format.
    RHI::ShaderDataType VertexFormat(u32 typeId) const
    {
        const TypeInfo* type = Type(typeId);
        if (type == nullptr)
        {
            return RHI::ShaderDataType::None;
        }

        if (type->Which == TypeInfo::Kind::Vector)
        {
            const TypeInfo* component = Type(type->ComponentType);
            if (component == nullptr)
            {
                return RHI::ShaderDataType::None;
            }
            const RHI::ShaderDataType scalar = ScalarType(*component);
            switch (type->Count)
            {
                case 2:
                    return scalar == RHI::ShaderDataType::Float    ? RHI::ShaderDataType::Float2
                           : scalar == RHI::ShaderDataType::Int    ? RHI::ShaderDataType::Int2
                                                                   : RHI::ShaderDataType::None;
                case 3:
                    return scalar == RHI::ShaderDataType::Float    ? RHI::ShaderDataType::Float3
                           : scalar == RHI::ShaderDataType::Int    ? RHI::ShaderDataType::Int3
                                                                   : RHI::ShaderDataType::None;
                case 4:
                    return scalar == RHI::ShaderDataType::Float    ? RHI::ShaderDataType::Float4
                           : scalar == RHI::ShaderDataType::Int    ? RHI::ShaderDataType::Int4
                                                                   : RHI::ShaderDataType::None;
                default:
                    return RHI::ShaderDataType::None;
            }
        }
        return ScalarType(*type);
    }

    ShaderResourceKind BindingKindOf(u32 storageClass, u32 typeId) const
    {
        const TypeInfo* type = Pointee(typeId);
        if (type == nullptr)
        {
            return ShaderResourceKind::Other;
        }

        if (storageClass == StorageClassPushConstant)
        {
            return ShaderResourceKind::PushConstant;
        }
        if (storageClass == StorageClassStorageBuffer)
        {
            return ShaderResourceKind::StorageBuffer;
        }
        if (storageClass == StorageClassUniform)
        {
            return ShaderResourceKind::UniformBuffer;
        }

        switch (type->Which)
        {
            case TypeInfo::Kind::SampledImage:
                return ShaderResourceKind::SampledImage;
            case TypeInfo::Kind::Sampler:
                return ShaderResourceKind::Sampler;
            case TypeInfo::Kind::Image:
                return type->Sampled == 2 ? ShaderResourceKind::StorageImage : ShaderResourceKind::SampledImage;
            default:
                return ShaderResourceKind::Other;
        }
    }

    /// Element count of an array binding: 0 when not an array, 1 when runtime-sized.
    u32 ArraySizeOf(u32 typeId) const
    {
        const TypeInfo* type = Pointee(typeId);
        if (type == nullptr)
        {
            return 0;
        }
        if (type->Which == TypeInfo::Kind::RuntimeArray)
        {
            return 1;
        }
        if (type->Which == TypeInfo::Kind::Array)
        {
            return ConstantValue(type->LengthId);
        }
        return 0;
    }
};

bool ParseInstructions(const u32* words, std::size_t wordCount, Reader& reader, std::string& error)
{
    std::size_t index = 5; // past the 5-word header
    while (index < wordCount)
    {
        const u32 instruction = words[index];
        const u32 count = instruction >> 16;
        const u16 opcode = static_cast<u16>(instruction & 0xFFFFu);
        if (count == 0 || index + count > wordCount)
        {
            error = "truncated instruction";
            return false;
        }

        const u32* operand = words + index + 1;
        const u32 operandCount = count - 1;

        switch (opcode)
        {
            case OpName:
                if (operandCount >= 2)
                {
                    reader.Names[operand[0]] = ReadString(operand, operandCount, 1);
                }
                break;

            case OpEntryPoint:
                if (operandCount >= 3)
                {
                    EntryPoint entry;
                    entry.ExecutionModel = operand[0];
                    // OpEntryPoint <execution model> <entry point id> "<name>" <interface...>
                    entry.Name = ReadString(operand, operandCount, 2);

                    // The name is NUL-terminated and padded to a word, so the
                    // interface ids start after the word that holds the NUL.
                    u32 firstInterfaceWord = 2;
                    while (firstInterfaceWord < operandCount)
                    {
                        const u32 packed = operand[firstInterfaceWord];
                        ++firstInterfaceWord;
                        if ((packed & 0xFFu) == 0 || (packed & 0xFF00u) == 0 || (packed & 0xFF0000u) == 0 ||
                            (packed & 0xFF000000u) == 0)
                        {
                            break;
                        }
                    }
                    for (u32 i = firstInterfaceWord; i < operandCount; ++i)
                    {
                        entry.Interface.push_back(operand[i]);
                    }
                    reader.EntryPoints.push_back(std::move(entry));
                }
                break;

            case OpExecutionMode:
            case OpExecutionModeId:
                if (operandCount >= 2)
                {
                    reader.ExecutionModes.emplace_back(
                        operand[0], std::vector<u32>(operand + 1, operand + operandCount));
                }
                break;

            case OpTypeVoid:
            case OpTypeBool:
            case OpTypeInt:
            case OpTypeFloat:
            case OpTypeVector:
            case OpTypeMatrix:
            case OpTypeImage:
            case OpTypeSampler:
            case OpTypeSampledImage:
            case OpTypeArray:
            case OpTypeRuntimeArray:
            case OpTypeStruct:
            case OpTypePointer:
                if (operandCount >= 1)
                {
                    TypeInfo type;
                    switch (opcode)
                    {
                        case OpTypeVoid:
                            type.Which = TypeInfo::Kind::Void;
                            break;
                        case OpTypeBool:
                            type.Which = TypeInfo::Kind::Bool;
                            break;
                        case OpTypeInt:
                            type.Which = TypeInfo::Kind::Int;
                            if (operandCount >= 3)
                            {
                                type.Width = operand[1];
                                type.Signed = operand[2] != 0;
                            }
                            break;
                        case OpTypeFloat:
                            type.Which = TypeInfo::Kind::Float;
                            if (operandCount >= 2)
                            {
                                type.Width = operand[1];
                            }
                            break;
                        case OpTypeVector:
                            type.Which = TypeInfo::Kind::Vector;
                            if (operandCount >= 3)
                            {
                                type.ComponentType = operand[1];
                                type.Count = operand[2];
                            }
                            break;
                        case OpTypeMatrix:
                            type.Which = TypeInfo::Kind::Matrix;
                            if (operandCount >= 3)
                            {
                                type.ComponentType = operand[1];
                                type.Count = operand[2];
                            }
                            break;
                        case OpTypeImage:
                            type.Which = TypeInfo::Kind::Image;
                            if (operandCount >= 7)
                            {
                                type.Sampled = operand[6];
                            }
                            break;
                        case OpTypeSampler:
                            type.Which = TypeInfo::Kind::Sampler;
                            break;
                        case OpTypeSampledImage:
                            type.Which = TypeInfo::Kind::SampledImage;
                            if (operandCount >= 2)
                            {
                                type.ComponentType = operand[1];
                            }
                            break;
                        case OpTypeArray:
                            type.Which = TypeInfo::Kind::Array;
                            if (operandCount >= 3)
                            {
                                type.ComponentType = operand[1];
                                type.LengthId = operand[2];
                            }
                            break;
                        case OpTypeRuntimeArray:
                            type.Which = TypeInfo::Kind::RuntimeArray;
                            if (operandCount >= 2)
                            {
                                type.ComponentType = operand[1];
                            }
                            break;
                        case OpTypeStruct:
                            type.Which = TypeInfo::Kind::Struct;
                            type.Members.assign(operand + 1, operand + operandCount);
                            break;
                        case OpTypePointer:
                            type.Which = TypeInfo::Kind::Pointer;
                            if (operandCount >= 3)
                            {
                                type.StorageClass = operand[1];
                                type.ComponentType = operand[2];
                            }
                            break;
                        default:
                            break;
                    }
                    reader.Types[operand[0]] = std::move(type);
                }
                break;

            case OpConstant:
                if (operandCount >= 3)
                {
                    reader.Constants[operand[1]] = operand[2];
                }
                break;

            case OpVariable:
                if (operandCount >= 3)
                {
                    Variable variable;
                    variable.TypeId = operand[0];
                    variable.StorageClass = operand[2];
                    reader.Variables[operand[1]] = variable;
                }
                break;

            case OpDecorate:
                if (operandCount >= 2)
                {
                    Decoration& decoration = reader.Decorations[operand[0]];
                    const u32 kind = operand[1];
                    const u32 value = operandCount >= 3 ? operand[2] : 0;
                    switch (kind)
                    {
                        case DecorationDescriptorSet:
                            decoration.DescriptorSet = value;
                            decoration.HasDescriptorSet = true;
                            break;
                        case DecorationBinding:
                            decoration.Binding = value;
                            decoration.HasBinding = true;
                            break;
                        case DecorationLocation:
                            decoration.Location = value;
                            decoration.HasLocation = true;
                            break;
                        case DecorationArrayStride:
                            decoration.ArrayStride = value;
                            decoration.HasArrayStride = true;
                            break;
                        case DecorationBuiltIn:
                            decoration.BuiltIn = value;
                            break;
                        case DecorationBlock:
                            decoration.Block = true;
                            break;
                        case DecorationBufferBlock:
                            decoration.BufferBlock = true;
                            break;
                        default:
                            break;
                    }
                }
                break;

            case OpMemberDecorate:
                if (operandCount >= 3)
                {
                    MemberDecoration& decoration = reader.MemberDecorations[operand[0]][operand[1]];
                    const u32 kind = operand[2];
                    const u32 value = operandCount >= 4 ? operand[3] : 0;
                    switch (kind)
                    {
                        case DecorationOffset:
                            decoration.Offset = value;
                            decoration.HasOffset = true;
                            break;
                        case DecorationMatrixStride:
                            decoration.MatrixStride = value;
                            decoration.HasMatrixStride = true;
                            break;
                        case DecorationRowMajor:
                            decoration.RowMajor = true;
                            break;
                        case DecorationColMajor:
                            decoration.ColMajor = true;
                            break;
                        default:
                            break;
                    }
                }
                break;

            default:
                break;
        }

        index += count;
    }
    return true;
}
} // namespace

const ShaderBinding* ShaderReflection::FindBinding(u32 set, u32 binding) const
{
    for (const ShaderBinding& candidate : Bindings)
    {
        if (candidate.Set == set && candidate.Binding == binding)
        {
            return &candidate;
        }
    }
    return nullptr;
}

ShaderReflection ReflectSPIRV(const std::vector<u8>& spirv)
{
    return ReflectSPIRV(spirv.data(), spirv.size());
}

ShaderReflection ReflectSPIRV(const void* data, std::size_t size)
{
    ShaderReflection reflection;

    if (data == nullptr || size < 20)
    {
        reflection.Error = "not SPIR-V: blob is smaller than a module header";
        return reflection;
    }
    if ((reinterpret_cast<std::uintptr_t>(data) % 4) != 0)
    {
        reflection.Error = "not SPIR-V: blob is not 4-byte aligned";
        return reflection;
    }

    const u32* words = static_cast<const u32*>(data);
    const std::size_t wordCount = size / 4;
    if (words[0] != kSpirvMagic)
    {
        reflection.Error = "not SPIR-V: bad magic number";
        return reflection;
    }

    Reader reader;
    std::string parseError;
    if (!ParseInstructions(words, wordCount, reader, parseError))
    {
        reflection.Error = "malformed SPIR-V: " + parseError;
        return reflection;
    }

    if (reader.EntryPoints.empty())
    {
        reflection.Error = "SPIR-V module declares no entry point";
        return reflection;
    }

    // Slang emits one entry point per module, and the pipeline names it "main".
    const EntryPoint& entry = reader.EntryPoints.front();
    reflection.EntryPoint = entry.Name;
    reflection.Stage = StageOfExecutionModel(entry.ExecutionModel);

    for (const auto& [entryId, modeWords] : reader.ExecutionModes)
    {
        (void)entryId;
        if (modeWords.empty())
        {
            continue;
        }
        const u32 mode = modeWords[0];
        if (mode == ExecutionModeLocalSize && modeWords.size() >= 4)
        {
            reflection.LocalSize[0] = modeWords[1];
            reflection.LocalSize[1] = modeWords[2];
            reflection.LocalSize[2] = modeWords[3];
        }
        else if (mode == ExecutionModeLocalSizeId && modeWords.size() >= 4)
        {
            reflection.LocalSize[0] = reader.ConstantValue(modeWords[1]);
            reflection.LocalSize[1] = reader.ConstantValue(modeWords[2]);
            reflection.LocalSize[2] = reader.ConstantValue(modeWords[3]);
        }
    }

    const std::unordered_set<u32> interfaceIds(entry.Interface.begin(), entry.Interface.end());

    for (const auto& [id, variable] : reader.Variables)
    {
        const Decoration& decoration = reader.DecorationOf(id);

        if (variable.StorageClass == StorageClassPushConstant)
        {
            const TypeInfo* pointee = reader.Pointee(variable.TypeId);
            if (pointee != nullptr)
            {
                reflection.PushConstantSize = reader.SizeOf(variable.TypeId, 0, false, 0);
            }
            reflection.Bindings.push_back(ShaderBinding{.Set = 0,
                                                         .Binding = 0,
                                                         .Kind = ShaderResourceKind::PushConstant,
                                                         .Name = reader.NameOf(id),
                                                         .ArraySize = 0});
            continue;
        }

        if (variable.StorageClass == StorageClassInput)
        {
            // Only the entry point's own inputs, and only real vertex attributes:
            // built-ins (gl_VertexIndex, gl_FragCoord) carry no location.
            if (!interfaceIds.contains(id) || decoration.BuiltIn != kNoDecoration || !decoration.HasLocation)
            {
                continue;
            }
            const TypeInfo* pointerType = reader.Type(variable.TypeId);
            const TypeInfo* pointee = reader.Pointee(variable.TypeId);
            if (pointerType == nullptr || pointee == nullptr)
            {
                continue;
            }
            reflection.VertexInputs.push_back(ShaderVertexInput{.Location = decoration.Location,
                                                                .Type = reader.VertexFormat(
                                                                    pointerType->ComponentType),
                                                                .Name = reader.NameOf(id)});
            continue;
        }

        const bool isResource = variable.StorageClass == StorageClassUniform ||
                                variable.StorageClass == StorageClassStorageBuffer ||
                                variable.StorageClass == StorageClassUniformConstant;
        if (!isResource || !decoration.HasBinding)
        {
            continue;
        }

        reflection.Bindings.push_back(ShaderBinding{
            .Set = decoration.DescriptorSet,
            .Binding = decoration.Binding,
            .Kind = reader.BindingKindOf(variable.StorageClass, variable.TypeId),
            .Name = reader.NameOf(id),
            .ArraySize = reader.ArraySizeOf(variable.TypeId)});
    }

    std::sort(reflection.VertexInputs.begin(), reflection.VertexInputs.end(),
              [](const ShaderVertexInput& a, const ShaderVertexInput& b) { return a.Location < b.Location; });
    std::sort(reflection.Bindings.begin(), reflection.Bindings.end(),
              [](const ShaderBinding& a, const ShaderBinding& b)
              {
                  if (a.Set != b.Set)
                  {
                      return a.Set < b.Set;
                  }
                  return a.Binding < b.Binding;
              });

    reflection.Valid = true;
    return reflection;
}
} // namespace UHE
