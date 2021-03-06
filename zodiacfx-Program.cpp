/*
Copyright 2019 Northbound Networks.

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
*/

#include <chrono>
#include <ctime>

#include "zodiacfx-Program.h"
#include "zodiacfx-Type.h"
#include "zodiacfx-Control.h"
#include "zodiacfx-Parser.h"
#include "zodiacfx-Table.h"
#include "frontends/p4/coreLibrary.h"
#include "frontends/common/options.h"

namespace ZODIACFX {

bool ZODIACFXProgram::build() {
    auto pack = toplevel->getMain();
    if (pack->type->name != "ZodiacfxSwitch")
        ::warning(ErrorType::WARN_INVALID, "%1%: the main zodiacfx package should be called ZodiacfxSwitch"
                  "; are you using the wrong architecture?", pack->type->name);

    if (pack->getConstructorParameters()->size() != 3) {
        ::error("Expected toplevel package %1% to have 3 parameters", pack->type);
        return false;
    }

    auto pb = pack->getParameterValue(model.zfx_switch.zfx_parser.name)->to<IR::ParserBlock>();
    BUG_CHECK(pb != nullptr, "No parser block found");
    parser = new ZODIACFXParser(this, pb, typeMap);
    bool success = parser->build();
    if (!success)
        return success;

    auto cb = pack->getParameterValue(model.zfx_switch.zfx_switch.name)->to<IR::ControlBlock>();
    BUG_CHECK(cb != nullptr, "No control block found");
    control = new ZODIACFXControl(this, cb, parser->headers);
    success = control->build();
    if (!success)
        return success;

    auto db = pack->getParameterValue(model.zfx_switch.zfx_deparser.name)->to<IR::ControlBlock>();
    BUG_CHECK(db != nullptr, "No deparser block found");
    deparser = new ZODIACFXDeparser(this, db, parser->headers);
    success = deparser->build();
    if (!success)
        return success;

    return true;
}

void ZODIACFXProgram::emitC(CodeBuilder* builder, cstring header) {
    emitGeneratedComment(builder);

    builder->appendFormat("#include \"%s\"", header.c_str());
    builder->newline();

    builder->target->emitIncludes(builder);
    emitPreamble(builder);
  
    builder->newline();
    builder->emitIndent(); 
    builder->target->emitCodeSection(builder, functionName);
    builder->emitIndent();
    builder->target->emitMain(builder, "packet_in", model.CPacketName.str(), "zodiacfx_ul_size");
    builder->blockStart();

    builder->newline();
    emitHeaderInstances(builder);
    builder->append(" = ");
    parser->headerType->emitInitializer(builder);
    builder->endOfStatement(true);

    emitLocalVariables(builder);
    builder->newline();
    builder->emitIndent();
    builder->appendFormat("goto %s;", IR::ParserState::start.c_str());
    builder->newline();

    builder->appendFormat("\n// Start of Parser\n");
    parser->emit(builder);

    builder->appendFormat("\n// Start of Pipeline\n");
    emitPipeline(builder);

    builder->appendFormat("\n// Start of Deparser\n");
    deparser->emit(builder);
    builder->appendFormat("gmac_write(%s, %s, %s.%s);", model.CPacketName.str(), inPacketLengthVar.c_str(),getSwitch()->outputMeta->name.name, ZODIACFXModel::instance.outputMetadataModel.outputPort.str());
    builder->newline();
    builder->blockEnd(true);  // end of function
    builder->target->emitLicense(builder, license);
}

void ZODIACFXProgram::emitH(CodeBuilder* builder, cstring) {
    emitGeneratedComment(builder);
    builder->appendLine("#ifndef _P4_GEN_HEADER_");
    builder->appendLine("#define _P4_GEN_HEADER_");
    builder->target->emitIncludes(builder);
    builder->newline();
    builder->appendLine("void packet_in(uint8_t *p_uc_data, uint16_t zodiacfx_ul_size, uint8_t port);");
    builder->newline();
    emitTypes(builder);
    control->emitTableTypes(builder);
    builder->appendLine("#endif");
}

void ZODIACFXProgram::emitGeneratedComment(CodeBuilder* builder) {
    std::chrono::time_point<std::chrono::system_clock> now = std::chrono::system_clock::now();
    std::time_t time = std::chrono::system_clock::to_time_t(now);
    builder->append("/* Automatically generated by ");
    builder->append(options.exe_name);
    builder->append(" from ");
    builder->append(options.file);
    builder->append(" on ");
    builder->append(std::ctime(&time));
    builder->append(" */");
    builder->newline();
}

void ZODIACFXProgram::emitTypes(CodeBuilder* builder) {
    for (auto d : program->objects) {
        if (d->is<IR::Type>() && !d->is<IR::IContainer>() &&
            !d->is<IR::Type_Extern>() && !d->is<IR::Type_Parser>() &&
            !d->is<IR::Type_Control>() && !d->is<IR::Type_Typedef>() &&
            !d->is<IR::Type_Error>()) {
            auto type = ZODIACFXTypeFactory::instance->create(d->to<IR::Type>());
            if (type == nullptr)
                continue;
            type->emit(builder);
            builder->newline();
        }
    }
}

void ZODIACFXProgram::emitPreamble(CodeBuilder* builder) {
    builder->emitIndent();
    builder->newline();
    builder->appendLine("#define ZODIACFX_MASK(t, w) ((((t)(1)) << (w)) - (t)1)");
    builder->appendLine("#define BYTES(w) ((w) / 8)");
    builder->newline();
}

void ZODIACFXProgram::emitLocalVariables(CodeBuilder* builder) {
    builder->newline();
    builder->emitIndent();
    builder->appendFormat("uint16_t %s = 0;", offsetVar); 
    builder->newline();
    builder->emitIndent();
    builder->appendFormat("uint8_t *%s = %s;", packetStartVar, model.CPacketName.str());    

    builder->newline();
    builder->emitIndent();
    builder->appendFormat("struct %s %s;\n", model.outputMetadataModel.name, getSwitch()->outputMeta->name.name);
    builder->emitIndent();
    builder->appendFormat("struct %s %s;\n", model.inputMetadataModel.name, getSwitch()->inputMeta->name.name);
    builder->emitIndent();
    builder->appendFormat("%s.%s = port;\n", getSwitch()->inputMeta->name.name, ZODIACFXModel::instance.inputMetadataModel.inputPort.str());
    
}

void ZODIACFXProgram::emitHeaderInstances(CodeBuilder* builder) {
    builder->emitIndent();
    parser->headerType->declare(builder, parser->headers->name.name, false);
}

void ZODIACFXProgram::emitPipeline(CodeBuilder* builder) {
    builder->emitIndent();
    builder->append(IR::ParserState::accept);
    builder->append(":");
    builder->newline();
    builder->emitIndent();
    builder->blockStart();
    control->emit(builder);
    builder->blockEnd(true);
}

ZODIACFXControl* ZODIACFXProgram::getSwitch() const {
    return dynamic_cast<ZODIACFXControl*>(control);
}

}  // namespace ZODIACFX
