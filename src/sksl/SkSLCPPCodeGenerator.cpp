/*
 * Copyright 2016 Google Inc.
 *
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

#include "SkSLCPPCodeGenerator.h"

#include "SkSLCompiler.h"
#include "SkSLHCodeGenerator.h"

namespace SkSL {

static bool needs_uniform_var(const Variable& var) {
    return (var.fModifiers.fFlags & Modifiers::kUniform_Flag) &&
           var.fType.fName != "colorSpaceXform";
}

CPPCodeGenerator::CPPCodeGenerator(const Context* context, const Program* program,
                                   ErrorReporter* errors, String name, OutputStream* out)
: INHERITED(context, program, errors, out)
, fName(std::move(name))
, fFullName(String::printf("Gr%s", fName.c_str()))
, fSectionAndParameterHelper(*program, *errors) {
    fLineEnding = "\\n";
}

void CPPCodeGenerator::writef(const char* s, va_list va) {
    static constexpr int BUFFER_SIZE = 1024;
    va_list copy;
    va_copy(copy, va);
    char buffer[BUFFER_SIZE];
    int length = vsnprintf(buffer, BUFFER_SIZE, s, va);
    if (length < BUFFER_SIZE) {
        fOut->write(buffer, length);
    } else {
        std::unique_ptr<char[]> heap(new char[length + 1]);
        vsprintf(heap.get(), s, copy);
        fOut->write(heap.get(), length);
    }
}

void CPPCodeGenerator::writef(const char* s, ...) {
    va_list va;
    va_start(va, s);
    this->writef(s, va);
    va_end(va);
}

void CPPCodeGenerator::writeHeader() {
}

bool CPPCodeGenerator::usesPrecisionModifiers() const {
    return false;
}

String CPPCodeGenerator::getTypeName(const Type& type) {
    return type.name();
}

void CPPCodeGenerator::writeBinaryExpression(const BinaryExpression& b,
                                             Precedence parentPrecedence) {
    if (b.fOperator == Token::PERCENT) {
        // need to use "%%" instead of "%" b/c the code will be inside of a printf
        Precedence precedence = GetBinaryPrecedence(b.fOperator);
        if (precedence >= parentPrecedence) {
            this->write("(");
        }
        this->writeExpression(*b.fLeft, precedence);
        this->write(" %% ");
        this->writeExpression(*b.fRight, precedence);
        if (precedence >= parentPrecedence) {
            this->write(")");
        }
    } else {
        INHERITED::writeBinaryExpression(b, parentPrecedence);
    }
}

void CPPCodeGenerator::writeIndexExpression(const IndexExpression& i) {
    const Expression& base = *i.fBase;
    if (base.fKind == Expression::kVariableReference_Kind) {
        int builtin = ((VariableReference&) base).fVariable.fModifiers.fLayout.fBuiltin;
        if (SK_TRANSFORMEDCOORDS2D_BUILTIN == builtin) {
            this->write("%s");
            if (i.fIndex->fKind != Expression::kIntLiteral_Kind) {
                fErrors.error(i.fIndex->fOffset,
                              "index into sk_TransformedCoords2D must be an integer literal");
                return;
            }
            int64_t index = ((IntLiteral&) *i.fIndex).fValue;
            String name = "sk_TransformedCoords2D_" + to_string(index);
            fFormatArgs.push_back(name + ".c_str()");
            if (fWrittenTransformedCoords.find(index) == fWrittenTransformedCoords.end()) {
                fExtraEmitCodeCode += "        SkString " + name +
                                      " = fragBuilder->ensureCoords2D(args.fTransformedCoords[" +
                                      to_string(index) + "]);\n";
                fWrittenTransformedCoords.insert(index);
            }
            return;
        } else if (SK_TEXTURESAMPLERS_BUILTIN == builtin) {
            this->write("%s");
            if (i.fIndex->fKind != Expression::kIntLiteral_Kind) {
                fErrors.error(i.fIndex->fOffset,
                              "index into sk_TextureSamplers must be an integer literal");
                return;
            }
            int64_t index = ((IntLiteral&) *i.fIndex).fValue;
            fFormatArgs.push_back("        fragBuilder->getProgramBuilder()->samplerVariable("
                                            "args.fTexSamplers[" + to_string(index) + "]).c_str()");
            return;
        }
    }
    INHERITED::writeIndexExpression(i);
}

static String default_value(const Type& type) {
    if (type.fName == "colorSpaceXform") {
        return "float4x4(1.0)";
    }
    switch (type.kind()) {
        case Type::kScalar_Kind: return "0";
        case Type::kVector_Kind: return type.name() + "(0)";
        case Type::kMatrix_Kind: return type.name() + "(1)";
        default: ABORT("unsupported default_value type\n");
    }
}

static bool is_private(const Variable& var) {
    return !(var.fModifiers.fFlags & Modifiers::kUniform_Flag) &&
           !(var.fModifiers.fFlags & Modifiers::kIn_Flag) &&
           var.fStorage == Variable::kGlobal_Storage &&
           var.fModifiers.fLayout.fBuiltin == -1;
}

void CPPCodeGenerator::writeRuntimeValue(const Type& type, const String& cppCode) {
    if (type.isFloat()) {
        this->write("%f");
        fFormatArgs.push_back(cppCode);
    } else if (type == *fContext.fInt_Type) {
        this->write("%d");
        fFormatArgs.push_back(cppCode);
    } else if (type == *fContext.fBool_Type) {
        this->write("%s");
        fFormatArgs.push_back("(" + cppCode + " ? \"true\" : \"false\")");
    } else if (type == *fContext.fFloat2_Type || type == *fContext.fHalf2_Type) {
        this->write(type.name() + "(%f, %f)");
        fFormatArgs.push_back(cppCode + ".fX");
        fFormatArgs.push_back(cppCode + ".fY");
    } else {
        this->write(type.name());
        this->write("\n");
        ABORT("unsupported runtime value type\n");
    }
}

void CPPCodeGenerator::writeVarInitializer(const Variable& var, const Expression& value) {
    if (is_private(var)) {
        this->writeRuntimeValue(var.fType, var.fName);
    } else {
        this->writeExpression(value, kTopLevel_Precedence);
    }
}

String CPPCodeGenerator::getSamplerHandle(const Variable& var) {
    int samplerCount = 0;
    for (const auto param : fSectionAndParameterHelper.getParameters()) {
        if (&var == param) {
            return "args.fTexSamplers[" + to_string(samplerCount) + "]";
        }
        if (param->fType.kind() == Type::kSampler_Kind) {
            ++samplerCount;
        }
    }
    ABORT("should have found sampler in parameters\n");
}

void CPPCodeGenerator::writeIntLiteral(const IntLiteral& i) {
    this->write(to_string((int32_t) i.fValue));
}

void CPPCodeGenerator::writeVariableReference(const VariableReference& ref) {
    switch (ref.fVariable.fModifiers.fLayout.fBuiltin) {
        case SK_INCOLOR_BUILTIN:
            this->write("%s");
            fFormatArgs.push_back(String("args.fInputColor ? args.fInputColor : \"half4(1)\""));
            break;
        case SK_OUTCOLOR_BUILTIN:
            this->write("%s");
            fFormatArgs.push_back(String("args.fOutputColor"));
            break;
        default:
            if (ref.fVariable.fType.kind() == Type::kSampler_Kind) {
                this->write("%s");
                fFormatArgs.push_back("fragBuilder->getProgramBuilder()->samplerVariable(" +
                                      this->getSamplerHandle(ref.fVariable) + ").c_str()");
                return;
            }
            if (ref.fVariable.fModifiers.fFlags & Modifiers::kUniform_Flag) {
                this->write("%s");
                String name = ref.fVariable.fName;
                String var;
                if (ref.fVariable.fType == *fContext.fColorSpaceXform_Type) {
                    ASSERT(fNeedColorSpaceHelper);
                    var = String::printf("fColorSpaceHelper.isValid() ? "
                                         "args.fUniformHandler->getUniformCStr("
                                                  "fColorSpaceHelper.gamutXformUniform()) : \"%s\"",
                           default_value(ref.fVariable.fType).c_str());
                } else {
                    var = String::printf("args.fUniformHandler->getUniformCStr(%sVar)",
                                         HCodeGenerator::FieldName(name.c_str()).c_str());
                }
                String code;
                if (ref.fVariable.fModifiers.fLayout.fWhen.size()) {
                    code = String::printf("%sVar.isValid() ? %s : \"%s\"",
                                          HCodeGenerator::FieldName(name.c_str()).c_str(),
                                          var.c_str(),
                                          default_value(ref.fVariable.fType).c_str());
                } else {
                    code = var;
                }
                fFormatArgs.push_back(code);
            } else if (SectionAndParameterHelper::IsParameter(ref.fVariable)) {
                String name(ref.fVariable.fName);
                this->writeRuntimeValue(ref.fVariable.fType,
                                        String::printf("_outer.%s()", name.c_str()).c_str());
            } else {
                this->write(ref.fVariable.fName);
            }
    }
}

void CPPCodeGenerator::writeIfStatement(const IfStatement& s) {
    if (s.fIsStatic) {
        this->write("@");
    }
    INHERITED::writeIfStatement(s);
}

void CPPCodeGenerator::writeSwitchStatement(const SwitchStatement& s) {
    if (s.fIsStatic) {
        this->write("@");
    }
    INHERITED::writeSwitchStatement(s);
}

void CPPCodeGenerator::writeFunctionCall(const FunctionCall& c) {
    if (c.fFunction.fBuiltin && c.fFunction.fName == "COLORSPACE") {
        String tmpVar = "_tmpVar" + to_string(++fVarCount);
        fFunctionHeader += "half4 " + tmpVar + ";";
        ASSERT(c.fArguments.size() == 2);
        this->write("%s");
        fFormatArgs.push_back("fColorSpaceHelper.isValid() ? \"(" + tmpVar + " = \" : \"\"");
        this->writeExpression(*c.fArguments[0], kTopLevel_Precedence);
        ASSERT(c.fArguments[1]->fKind == Expression::kVariableReference_Kind);
        String xform("args.fUniformHandler->getUniformCStr(fColorSpaceHelper.gamutXformUniform())");
        this->write("%s");
        fFormatArgs.push_back("fColorSpaceHelper.isValid() ? SkStringPrintf(\", "
                              "half4(clamp((%s * half4(" + tmpVar + ".rgb, 1.0)).rgb, 0.0, " +
                              tmpVar + ".a), " + tmpVar + ".a))\", " + xform + ").c_str() : \"\"");
        return;
    }
    INHERITED::writeFunctionCall(c);
    if (c.fFunction.fBuiltin && c.fFunction.fName == "texture") {
        this->write(".%s");
        ASSERT(c.fArguments.size() >= 1);
        ASSERT(c.fArguments[0]->fKind == Expression::kVariableReference_Kind);
        String sampler = this->getSamplerHandle(((VariableReference&) *c.fArguments[0]).fVariable);
        fFormatArgs.push_back("fragBuilder->getProgramBuilder()->samplerSwizzle(" + sampler +
                              ").c_str()");
    }
}

void CPPCodeGenerator::writeFunction(const FunctionDefinition& f) {
    if (f.fDeclaration.fName == "main") {
        fFunctionHeader = "";
        OutputStream* oldOut = fOut;
        StringStream buffer;
        fOut = &buffer;
        for (const auto& s : ((Block&) *f.fBody).fStatements) {
            this->writeStatement(*s);
            this->writeLine();
        }

        fOut = oldOut;
        this->write(fFunctionHeader);
        this->write(buffer.str());
    } else {
        INHERITED::writeFunction(f);
    }
}

void CPPCodeGenerator::writeSetting(const Setting& s) {
    static constexpr const char* kPrefix = "sk_Args.";
    if (!strncmp(s.fName.c_str(), kPrefix, strlen(kPrefix))) {
        const char* name = s.fName.c_str() + strlen(kPrefix);
        this->writeRuntimeValue(s.fType, HCodeGenerator::FieldName(name).c_str());
    } else {
        this->write(s.fName.c_str());
    }
}

bool CPPCodeGenerator::writeSection(const char* name, const char* prefix) {
    const Section* s = fSectionAndParameterHelper.getSection(name);
    if (s) {
        this->writef("%s%s", prefix, s->fText.c_str());
        return true;
    }
    return false;
}

void CPPCodeGenerator::writeProgramElement(const ProgramElement& p) {
    if (p.fKind == ProgramElement::kSection_Kind) {
        return;
    }
    if (p.fKind == ProgramElement::kVar_Kind) {
        const VarDeclarations& decls = (const VarDeclarations&) p;
        if (!decls.fVars.size()) {
            return;
        }
        const Variable& var = *((VarDeclaration&) *decls.fVars[0]).fVar;
        if (var.fModifiers.fFlags & (Modifiers::kIn_Flag | Modifiers::kUniform_Flag) ||
            -1 != var.fModifiers.fLayout.fBuiltin) {
            return;
        }
    }
    INHERITED::writeProgramElement(p);
}

void CPPCodeGenerator::addUniform(const Variable& var) {
    if (!needs_uniform_var(var)) {
        return;
    }
    const char* precision;
    if (var.fModifiers.fFlags & Modifiers::kHighp_Flag) {
        precision = "kHigh_GrSLPrecision";
    } else if (var.fModifiers.fFlags & Modifiers::kMediump_Flag) {
        precision = "kMedium_GrSLPrecision";
    } else if (var.fModifiers.fFlags & Modifiers::kLowp_Flag) {
        precision = "kLow_GrSLPrecision";
    } else {
        precision = "kDefault_GrSLPrecision";
    }
    const char* type;
    if (var.fType == *fContext.fFloat_Type) {
        type = "kFloat_GrSLType";
    } else if (var.fType == *fContext.fHalf_Type) {
        type = "kHalf_GrSLType";
    } else if (var.fType == *fContext.fFloat2_Type) {
        type = "kFloat2_GrSLType";
    } else if (var.fType == *fContext.fHalf2_Type) {
        type = "kHalf2_GrSLType";
    } else if (var.fType == *fContext.fFloat4_Type) {
        type = "kFloat4_GrSLType";
    } else if (var.fType == *fContext.fHalf4_Type) {
        type = "kHalf4_GrSLType";
    } else if (var.fType == *fContext.fFloat4x4_Type ||
               var.fType == *fContext.fColorSpaceXform_Type) {
        type = "kFloat4x4_GrSLType";
    } else if (var.fType == *fContext.fHalf4x4_Type) {
        type = "kHalf4x4_GrSLType";
    } else {
        ABORT("unsupported uniform type: %s %s;\n", String(var.fType.fName).c_str(),
              String(var.fName).c_str());
    }
    if (var.fModifiers.fLayout.fWhen.size()) {
        this->writef("        if (%s) {\n    ", var.fModifiers.fLayout.fWhen.c_str());
    }
    String name(var.fName);
    this->writef("        %sVar = args.fUniformHandler->addUniform(kFragment_GrShaderFlag, %s, "
                 "%s, \"%s\");\n", HCodeGenerator::FieldName(name.c_str()).c_str(), type, precision,
                 name.c_str());
    if (var.fModifiers.fLayout.fWhen.size()) {
        this->write("        }\n");
    }
}

void CPPCodeGenerator::writePrivateVars() {
    for (const auto& p : fProgram.fElements) {
        if (ProgramElement::kVar_Kind == p->fKind) {
            const VarDeclarations* decls = (const VarDeclarations*) p.get();
            for (const auto& raw : decls->fVars) {
                VarDeclaration& decl = (VarDeclaration&) *raw;
                if (is_private(*decl.fVar)) {
                    this->writef("%s %s;\n",
                                 HCodeGenerator::FieldType(decl.fVar->fType).c_str(),
                                 String(decl.fVar->fName).c_str());
                }
            }
        }
    }
}

void CPPCodeGenerator::writePrivateVarValues() {
    for (const auto& p : fProgram.fElements) {
        if (ProgramElement::kVar_Kind == p->fKind) {
            const VarDeclarations* decls = (const VarDeclarations*) p.get();
            for (const auto& raw : decls->fVars) {
                VarDeclaration& decl = (VarDeclaration&) *raw;
                if (is_private(*decl.fVar) && decl.fValue) {
                    this->writef("%s = %s;\n",
                                 String(decl.fVar->fName).c_str(),
                                 decl.fValue->description().c_str());
                }
            }
        }
    }
}

bool CPPCodeGenerator::writeEmitCode(std::vector<const Variable*>& uniforms) {
    this->write("    void emitCode(EmitArgs& args) override {\n"
                "        GrGLSLFPFragmentBuilder* fragBuilder = args.fFragBuilder;\n");
    this->writef("        const %s& _outer = args.fFp.cast<%s>();\n"
                 "        (void) _outer;\n",
                 fFullName.c_str(), fFullName.c_str());
    this->writePrivateVarValues();
    for (const auto u : uniforms) {
        this->addUniform(*u);
        if (u->fType == *fContext.fColorSpaceXform_Type) {
            if (fNeedColorSpaceHelper) {
                fErrors.error(u->fOffset, "only a single ColorSpaceXform is supported");
            }
            fNeedColorSpaceHelper = true;
            this->writef("        fColorSpaceHelper.emitCode(args.fUniformHandler, "
                                                            "_outer.%s().get());\n",
                         String(u->fName).c_str());
        }
    }
    this->writeSection(EMIT_CODE_SECTION);
    OutputStream* old = fOut;
    StringStream mainBuffer;
    fOut = &mainBuffer;
    bool result = INHERITED::generateCode();
    fOut = old;
    this->writef("%s        fragBuilder->codeAppendf(\"%s\"", fExtraEmitCodeCode.c_str(),
                 mainBuffer.str().c_str());
    for (const auto& s : fFormatArgs) {
        this->writef(", %s", s.c_str());
    }
    this->write(");\n"
                "    }\n");
    return result;
}

void CPPCodeGenerator::writeSetData(std::vector<const Variable*>& uniforms) {
    const char* fullName = fFullName.c_str();
    const Section* section = fSectionAndParameterHelper.getSection(SET_DATA_SECTION);
    const char* pdman = section ? section->fArgument.c_str() : "pdman";
    this->writef("    void onSetData(const GrGLSLProgramDataManager& %s, "
                                    "const GrFragmentProcessor& _proc) override {\n",
                 pdman);
    bool wroteProcessor = false;
    for (const auto u : uniforms) {
        if (u->fModifiers.fFlags & Modifiers::kIn_Flag) {
            if (!wroteProcessor) {
                this->writef("        const %s& _outer = _proc.cast<%s>();\n", fullName, fullName);
                wroteProcessor = true;
                this->writef("        {\n");
            }
            String nameString(u->fName);
            const char* name = nameString.c_str();
            if (u->fType == *fContext.fFloat4_Type || u->fType == *fContext.fHalf4_Type) {
                this->writef("        const SkRect %sValue = _outer.%s();\n"
                             "        %s.set4fv(%sVar, 1, (float*) &%sValue);\n",
                             name, name, pdman, HCodeGenerator::FieldName(name).c_str(), name);
            } else if (u->fType == *fContext.fFloat4x4_Type ||
                       u->fType == *fContext.fHalf4x4_Type) {
                this->writef("        float %sValue[16];\n"
                             "        _outer.%s().asColMajorf(%sValue);\n"
                             "        %s.setMatrix4f(%sVar, %sValue);\n",
                             name, name, name, pdman, HCodeGenerator::FieldName(name).c_str(),
                             name);
            } else if (u->fType == *fContext.fColorSpaceXform_Type) {
                ASSERT(fNeedColorSpaceHelper);
                this->writef("        if (fColorSpaceHelper.isValid()) {\n"
                             "            fColorSpaceHelper.setData(%s, _outer.%s().get());\n"
                             "        }\n",
                             pdman, name);
            } else {
                this->writef("        %s.set1f(%sVar, _outer.%s());\n",
                             pdman, HCodeGenerator::FieldName(name).c_str(), name);
            }
        }
    }
    if (wroteProcessor) {
        this->writef("        }\n");
    }
    if (section) {
        for (const auto& p : fProgram.fElements) {
            if (ProgramElement::kVar_Kind == p->fKind) {
                const VarDeclarations* decls = (const VarDeclarations*) p.get();
                for (const auto& raw : decls->fVars) {
                    VarDeclaration& decl = (VarDeclaration&) *raw;
                    String nameString(decl.fVar->fName);
                    const char* name = nameString.c_str();
                    if (needs_uniform_var(*decl.fVar)) {
                        this->writef("        UniformHandle& %s = %sVar;\n"
                                     "        (void) %s;\n",
                                     name, HCodeGenerator::FieldName(name).c_str(), name);
                    } else if (SectionAndParameterHelper::IsParameter(*decl.fVar)) {
                        if (!wroteProcessor) {
                            this->writef("        const %s& _outer = _proc.cast<%s>();\n", fullName,
                                         fullName);
                            wroteProcessor = true;
                        }
                        this->writef("        auto %s = _outer.%s();\n"
                                     "        (void) %s;\n",
                                     name, name, name);
                    }
                }
            }
        }
        this->writeSection(SET_DATA_SECTION);
    }
    this->write("    }\n");
}

void CPPCodeGenerator::writeClone() {
    if (!this->writeSection(CLONE_SECTION)) {
        if (fSectionAndParameterHelper.getSection(FIELDS_SECTION)) {
            fErrors.error(0, "fragment processors with custom @fields must also have a custom"
                             "@clone");
        }
        this->writef("%s::%s(const %s& src)\n"
                     ": INHERITED(k%s_ClassID, src.optimizationFlags())", fFullName.c_str(),
                     fFullName.c_str(), fFullName.c_str(), fFullName.c_str());
        for (const auto& param : fSectionAndParameterHelper.getParameters()) {
            String fieldName = HCodeGenerator::FieldName(String(param->fName).c_str());
            this->writef("\n, %s(%s)",
                         fieldName.c_str(),
                         ("src." + fieldName).c_str());
        }
        for (const Section* s : fSectionAndParameterHelper.getSections(COORD_TRANSFORM_SECTION)) {
            String fieldName = HCodeGenerator::FieldName(s->fArgument.c_str());
            this->writef("\n, %sCoordTransform(src.%sCoordTransform)", fieldName.c_str(),
                         fieldName.c_str());
        }
        this->writef(" {\n");
        for (const auto& param : fSectionAndParameterHelper.getParameters()) {
            if (param->fType.kind() == Type::kSampler_Kind) {
                this->writef("    this->addTextureSampler(&%s);\n",
                             HCodeGenerator::FieldName(String(param->fName).c_str()).c_str());
            }
        }
        for (const Section* s : fSectionAndParameterHelper.getSections(COORD_TRANSFORM_SECTION)) {
            String field = HCodeGenerator::FieldName(s->fArgument.c_str());
            this->writef("    this->addCoordTransform(&%sCoordTransform);\n", field.c_str());
        }
        this->write("}\n");
        this->writef("std::unique_ptr<GrFragmentProcessor> %s::clone() const {\n",
                     fFullName.c_str());
        this->writef("    return std::unique_ptr<GrFragmentProcessor>(new %s(*this));\n",
                     fFullName.c_str());
        this->write("}\n");
    }
}

void CPPCodeGenerator::writeTest() {
    const Section* test = fSectionAndParameterHelper.getSection(TEST_CODE_SECTION);
    if (test) {
        this->writef(
                "GR_DEFINE_FRAGMENT_PROCESSOR_TEST(%s);\n"
                "#if GR_TEST_UTILS\n"
                "std::unique_ptr<GrFragmentProcessor> %s::TestCreate(GrProcessorTestData* %s) {\n",
                fFullName.c_str(),
                fFullName.c_str(),
                test->fArgument.c_str());
        this->writeSection(TEST_CODE_SECTION);
        this->write("}\n"
                    "#endif\n");
    }
}

void CPPCodeGenerator::writeGetKey() {
    this->writef("void %s::onGetGLSLProcessorKey(const GrShaderCaps& caps, "
                                                "GrProcessorKeyBuilder* b) const {\n",
                 fFullName.c_str());
    for (const auto& param : fSectionAndParameterHelper.getParameters()) {
        String nameString(param->fName);
        const char* name = nameString.c_str();
        if (param->fType == *fContext.fColorSpaceXform_Type) {
            this->writef("    b->add32(GrColorSpaceXform::XformKey(%s.get()));\n",
                         HCodeGenerator::FieldName(name).c_str());
            continue;
        }
        if (param->fModifiers.fLayout.fKey != Layout::kNo_Key &&
            (param->fModifiers.fFlags & Modifiers::kUniform_Flag)) {
            fErrors.error(param->fOffset,
                          "layout(key) may not be specified on uniforms");
        }
        switch (param->fModifiers.fLayout.fKey) {
            case Layout::kKey_Key:
                if (param->fType == *fContext.fFloat4x4_Type) {
                    ABORT("no automatic key handling for float4x4\n");
                } else if (param->fType == *fContext.fFloat2_Type) {
                    this->writef("    b->add32(%s.fX);\n",
                                 HCodeGenerator::FieldName(name).c_str());
                    this->writef("    b->add32(%s.fY);\n",
                                 HCodeGenerator::FieldName(name).c_str());
                } else if (param->fType == *fContext.fFloat4_Type) {
                    this->writef("    b->add32(%s.x());\n",
                                 HCodeGenerator::FieldName(name).c_str());
                    this->writef("    b->add32(%s.y());\n",
                                 HCodeGenerator::FieldName(name).c_str());
                    this->writef("    b->add32(%s.width());\n",
                                 HCodeGenerator::FieldName(name).c_str());
                    this->writef("    b->add32(%s.height());\n",
                                 HCodeGenerator::FieldName(name).c_str());
                } else {
                    this->writef("    b->add32(%s);\n",
                                 HCodeGenerator::FieldName(name).c_str());
                }
                break;
            case Layout::kIdentity_Key:
                if (param->fType.kind() != Type::kMatrix_Kind) {
                    fErrors.error(param->fOffset,
                                  "layout(key=identity) requires matrix type");
                }
                this->writef("    b->add32(%s.isIdentity() ? 1 : 0);\n",
                             HCodeGenerator::FieldName(name).c_str());
                break;
            case Layout::kNo_Key:
                break;
        }
    }
    this->write("}\n");
}

bool CPPCodeGenerator::generateCode() {
    std::vector<const Variable*> uniforms;
    for (const auto& p : fProgram.fElements) {
        if (ProgramElement::kVar_Kind == p->fKind) {
            const VarDeclarations* decls = (const VarDeclarations*) p.get();
            for (const auto& raw : decls->fVars) {
                VarDeclaration& decl = (VarDeclaration&) *raw;
                if ((decl.fVar->fModifiers.fFlags & Modifiers::kUniform_Flag) &&
                           decl.fVar->fType.kind() != Type::kSampler_Kind) {
                    uniforms.push_back(decl.fVar);
                }
            }
        }
    }
    const char* baseName = fName.c_str();
    const char* fullName = fFullName.c_str();
    this->writef(kFragmentProcessorHeader, fullName);
    this->writef("#include \"%s.h\"\n"
                 "#if SK_SUPPORT_GPU\n", fullName);
    this->writeSection(CPP_SECTION);
    this->writef("#include \"glsl/GrGLSLColorSpaceXformHelper.h\"\n"
                 "#include \"glsl/GrGLSLFragmentProcessor.h\"\n"
                 "#include \"glsl/GrGLSLFragmentShaderBuilder.h\"\n"
                 "#include \"glsl/GrGLSLProgramBuilder.h\"\n"
                 "#include \"SkSLCPP.h\"\n"
                 "#include \"SkSLUtil.h\"\n"
                 "class GrGLSL%s : public GrGLSLFragmentProcessor {\n"
                 "public:\n"
                 "    GrGLSL%s() {}\n",
                 baseName, baseName);
    bool result = this->writeEmitCode(uniforms);
    this->write("private:\n");
    this->writeSetData(uniforms);
    this->writePrivateVars();
    for (const auto& u : uniforms) {
        if (needs_uniform_var(*u) && !(u->fModifiers.fFlags & Modifiers::kIn_Flag)) {
            this->writef("    UniformHandle %sVar;\n",
                         HCodeGenerator::FieldName(String(u->fName).c_str()).c_str());
        }
    }
    for (const auto& param : fSectionAndParameterHelper.getParameters()) {
        if (needs_uniform_var(*param)) {
            this->writef("    UniformHandle %sVar;\n",
                         HCodeGenerator::FieldName(String(param->fName).c_str()).c_str());
        }
    }
    if (fNeedColorSpaceHelper) {
        this->write("    GrGLSLColorSpaceXformHelper fColorSpaceHelper;\n");
    }
    this->writef("};\n"
                 "GrGLSLFragmentProcessor* %s::onCreateGLSLInstance() const {\n"
                 "    return new GrGLSL%s();\n"
                 "}\n",
                 fullName, baseName);
    this->writeGetKey();
    this->writef("bool %s::onIsEqual(const GrFragmentProcessor& other) const {\n"
                 "    const %s& that = other.cast<%s>();\n"
                 "    (void) that;\n",
                 fullName, fullName, fullName);
    for (const auto& param : fSectionAndParameterHelper.getParameters()) {
        String nameString(param->fName);
        const char* name = nameString.c_str();
        this->writef("    if (%s != that.%s) return false;\n",
                     HCodeGenerator::FieldName(name).c_str(),
                     HCodeGenerator::FieldName(name).c_str());
    }
    this->write("    return true;\n"
                "}\n");
    this->writeClone();
    this->writeTest();
    this->writeSection(CPP_END_SECTION);
    this->write("#endif\n");
    result &= 0 == fErrors.errorCount();
    return result;
}

} // namespace
