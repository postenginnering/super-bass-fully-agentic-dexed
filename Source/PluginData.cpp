/**
 *
 * Copyright (c) 2014-2025 Pascal Gauthier.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301  USA
 *
 */

#include <time.h>

#include "PluginParam.h"
#include "PluginProcessor.h"
#include "agent/AgentController.h"
#include "agent/context/PortablePresetContext.h"
#include "ui/UiOperationResult.h"
#include "PluginData.h"
#include "state/SynthStateService.h"
#include "Dexed.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <fstream>
#include <thread>

uint8_t sysexChecksum(const uint8_t *sysex, int size) {
    int sum = 0;
    int i;
    
    for (i = 0; i < size; sum -= sysex[i++]);
    return sum & 0x7F;
}

void exportSysexPgm(uint8_t *dest, uint8_t *src) {
    uint8_t header[] = { 0xF0, 0x43, 0x00, 0x00, 0x01, 0x1B };
   
    memcpy(dest, header, 6);
    
    // copy 1 unpacked voices
    memcpy(dest+6, src, 155);
        
    // make checksum for dump
    uint8_t footer[] = { sysexChecksum(src, 155), 0xF7 };
    
    memcpy(dest+161, footer, 2);
}

/**
 * Pack a program into a 32 packed sysex
 */
void Cartridge::packProgram(uint8_t *src, int idx, String name, char *opSwitch) {
    uint8_t *bulk = voiceData + 6 + (idx * 128);
    
    for(int op = 0; op < 6; op++) {
        // eg rate and level, brk pt, depth, scaling
        memcpy(bulk + op * 17, src + op * 21, 11);
        int pp = op*17;
        int up = op*21;
        
        // left curves
        bulk[pp+11] = (src[up+11]&0x03) | ((src[up+12]&0x03) << 2);
        bulk[pp+12] = (src[up+13]&0x07) | ((src[up+20]&0x0f) << 3);
        // kvs_ams
        bulk[pp+13] = (src[up+14]&0x03) | ((src[up+15]&0x07) << 2);
        // output lvl
        if ( opSwitch[op] == '0' )
            bulk[pp+14] = 0;
        else
            bulk[pp+14] = src[up+16];
        // fcoarse_mode
        bulk[pp+15] = (src[up+17]&0x01) | ((src[up+18]&0x1f) << 1);
        // fine freq
        bulk[pp+16] = src[up+19];
    }
    memcpy(bulk + 102, src + 126, 9);      // pitch env, algo
    bulk[111] = (src[135]&0x07) | ((src[136]&0x01) << 3);
    memcpy(bulk + 112, src + 137, 4);      // lfo
    bulk[116] = (src[141]&0x01) | (((src[142]&0x07) << 1) | ((src[143]&0x07) << 4));
    bulk[117] = src[144];
        
    int eos = 0;

    for(int i=0; i < 10; i++) {
        char c = (char) name[i];
        if ( c == 0 )
            eos = 1;
        if ( eos ) {
            bulk[118+i] = ' ';
            continue;
        }
        c = c < 32 ? ' ' : c;
        c = c > 127 ? ' ' : c;
        bulk[118+i] = c;
    }
}

/**
 * This function normalize data that comes from corrupted sysex.
 * It used to avoid engine crashing upon extreme values
 */
uint8_t normparm(uint8_t value, char max, int id) {
    if ( value <= max )
        return value;
    // if this is beyond the max, we expect a 0-255 range, normalize this
    // to the expected return value; and this value as a random data.
    return ((float)value)/255 * max;
}

void Cartridge::unpackProgram(uint8_t *unpackPgm, int idx) {
    // TODO put this in uint8_t :D
    char *bulk = (char *)voiceData + 6 + (idx * 128);
    
    for (int op = 0; op < 6; op++) {
        // eg rate and level, brk pt, depth, scaling
        
        for(int i=0; i<11; i++) {
            uint8_t currparm = bulk[op * 17 + i] & 0x7F; // mask BIT7 (don't care per sysex spec) 
            unpackPgm[op * 21 + i] = normparm(currparm, 99, i);
        }
        
        memcpy(unpackPgm + op * 21, bulk + op * 17, 11);
        char leftrightcurves = bulk[op * 17 + 11]&0xF; // bits 4-7 don't care per sysex spec
        unpackPgm[op * 21 + 11] = leftrightcurves & 3;
        unpackPgm[op * 21 + 12] = (leftrightcurves >> 2) & 3;
        char detune_rs = bulk[op * 17 + 12]&0x7F;
        unpackPgm[op * 21 + 13] = detune_rs & 7;
        char kvs_ams = bulk[op * 17 + 13]&0x1F; // bits 5-7 don't care per sysex spec
        unpackPgm[op * 21 + 14] = kvs_ams & 3;
        unpackPgm[op * 21 + 15] = (kvs_ams >> 2) & 7;
        unpackPgm[op * 21 + 16] = bulk[op * 17 + 14]&0x7F;  // output level
        char fcoarse_mode = bulk[op * 17 + 15]&0x3F; //bits 6-7 don't care per sysex spec
        unpackPgm[op * 21 + 17] = fcoarse_mode & 1;
        unpackPgm[op * 21 + 18] = (fcoarse_mode >> 1)&0x1F;
        unpackPgm[op * 21 + 19] = bulk[op * 17 + 16]&0x7F;  // fine freq
        unpackPgm[op * 21 + 20] = (detune_rs >> 3) &0x7F;
    }
    
    for (int i=0; i<8; i++)  {
        uint8_t currparm = bulk[102 + i] & 0x7F; // mask BIT7 (don't care per sysex spec)
        unpackPgm[126+i] = normparm(currparm, 99, 126+i);
    }
    unpackPgm[134] = normparm(bulk[110]&0x1F, 31, 134); // bits 5-7 are don't care per sysex spec
    
    char oks_fb = bulk[111]&0xF;//bits 4-7 are don't care per spec
    unpackPgm[135] = oks_fb & 7;
    unpackPgm[136] = oks_fb >> 3;
    unpackPgm[137] = bulk[112] & 0x7F; // lfs
    unpackPgm[138] = bulk[113] & 0x7F; // lfd
    unpackPgm[139] = bulk[114] & 0x7F; // lpmd
    unpackPgm[140] = bulk[115] & 0x7F; // lamd
    char lpms_lfw_lks = bulk[116] & 0x7F;
    unpackPgm[141] = lpms_lfw_lks & 1;
    unpackPgm[142] = (lpms_lfw_lks >> 1) & 7;
    unpackPgm[143] = lpms_lfw_lks >> 4;
    unpackPgm[144] = bulk[117] & 0x7F;
    for (int name_idx = 0; name_idx < 10; name_idx++) {
        unpackPgm[145 + name_idx] = bulk[118 + name_idx] & 0x7F;
    } //name_idx
//    memcpy(unpackPgm + 144, bulk + 117, 11);  // transpose, name
}

void DexedAudioProcessor::loadCartridge(Cartridge &sysex) {
    currentCart = sysex;
    currentCart.getProgramNames(programNames);
}

void DexedAudioProcessor::packOpSwitch() {
    char value = (controllers.opSwitch[5] == '1') << 5;
    value += (controllers.opSwitch[4] == '1') << 4;
    value += (controllers.opSwitch[3] == '1') << 3;
    value += (controllers.opSwitch[2] == '1') << 2;
    value += (controllers.opSwitch[1] == '1') << 1;
    value += (controllers.opSwitch[0] == '1');
    data[155] = value;
}

void DexedAudioProcessor::unpackOpSwitch(char packOpValue) {
    controllers.opSwitch[5] = ((packOpValue >> 5) &1) + 48;
    controllers.opSwitch[4] = ((packOpValue >> 4) &1) + 48;
    controllers.opSwitch[3] = ((packOpValue >> 3) &1) + 48;
    controllers.opSwitch[2] = ((packOpValue >> 2) &1) + 48;
    controllers.opSwitch[1] = ((packOpValue >> 1) &1) + 48;
    controllers.opSwitch[0] = (packOpValue &1) + 48;
}

std::string DexedAudioProcessor::agenticPatchName() const {
    const uint8_t* nameBytes = data + 145;
    agentic_dexed::RealtimeSynthState realtime;
    if (agenticParameterStore_ != nullptr)
    {
        while (!readAgenticRealtimeState(realtime))
            std::this_thread::yield();
        nameBytes = realtime.voiceBytes.data() + 145;
    }
    std::string name(reinterpret_cast<const char*>(nameBytes), 10);
    while (!name.empty() && (name.back() == ' ' || name.back() == '\0'))
        name.pop_back();
    return name;
}

void DexedAudioProcessor::setAgenticPatchName(const std::string& name) {
    std::fill(data + 145, data + 155, static_cast<uint8_t>(' '));
    const auto length = std::min<std::size_t>(name.size(), 10);
    std::copy_n(name.begin(), length, data + 145);
}

int DexedAudioProcessor::updateProgramFromSysex(const uint8_t *rawdata) {
    if (sysexChecksum(rawdata, 155) != rawdata[155])
        return 1;

    panic();
    memcpy(data, rawdata, 155);
    unpackOpSwitch(0x3F);
    lfo.reset(data + 137);
    publishLegacyStateToRealtimeStore();
    triggerAsyncUpdate();
    return 0;
}

void DexedAudioProcessor::setupStartupCart() {
    File startup = dexedCartDir.getChildFile("Dexed_01.syx");

    if ( currentCart.load(startup) != -1 ) {
        loadCartridge(currentCart);
        return;
    }
    
    // The user deleted the file :/, load from the builtin zip file.
    setupBuiltinCart();
}

void DexedAudioProcessor::setupBuiltinCart() {
    MemoryInputStream *mis = new MemoryInputStream(BinaryData::builtin_pgm_zip, BinaryData::builtin_pgm_zipSize, false);
    ZipFile *builtin_pgm = new ZipFile(mis, true);
    const auto entry = builtin_pgm->getIndexOfFileName(("Dexed_01.syx"));
    InputStream *is = entry >= 0 ? builtin_pgm->createStreamForEntry(entry) : nullptr;
    Cartridge init;

    if (is != nullptr && init.load(*is) != -1) {
        loadCartridge(init);
    }

    delete is;
    delete builtin_pgm;
}

void DexedAudioProcessor::resetToInitVoice() {
    const char init_voice[] =
      { 99, 99, 99, 99, 99, 99, 99, 00, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 0, 7,
        99, 99, 99, 99, 99, 99, 99, 00, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 0, 7,
        99, 99, 99, 99, 99, 99, 99, 00, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 0, 7,
        99, 99, 99, 99, 99, 99, 99, 00, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 0, 7,
        99, 99, 99, 99, 99, 99, 99, 00, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 0, 7,
        99, 99, 99, 99, 99, 99, 99, 00, 0, 0, 0, 0, 0, 0, 0, 0, 99, 0, 1, 0, 7,
        99, 99, 99, 99, 50, 50, 50, 50, 0, 0, 1, 35, 0, 0, 0, 1, 0, 3, 24,
        73, 78, 73, 84, 32, 86, 79, 73, 67, 69 };
    
    for(int i=0;i<sizeof(init_voice);i++) {
        data[i] = init_voice[i];
    }
    unpackOpSwitch(0x3F);
    publishLegacyStateToRealtimeStore();
    panic();
    triggerAsyncUpdate();
}

void DexedAudioProcessor::copyToClipboard(int srcOp) {
    DexedClipboard clipboard(data + (srcOp *21), 21);
    clipboard.write(String("Program: '") + getProgramName(getCurrentProgram()) + "' operator: " + String(6-srcOp));
}

void DexedAudioProcessor::pasteOpFromClipboard(int destOp) {
    DexedClipboard clipboard;

    jassert(clipboard.isOperatorData());

    memcpy(data+(destOp*21), clipboard.getRawData(), 21);
    publishLegacyStateToRealtimeStore();
    triggerAsyncUpdate();
}

void DexedAudioProcessor::pasteEnvFromClipboard(int destOp) {
    DexedClipboard clipboard;

    jassert(clipboard.isOperatorData());

    memcpy(data+(destOp*21), clipboard.getRawData(), 8);
    publishLegacyStateToRealtimeStore();
    triggerAsyncUpdate();
}

void DexedAudioProcessor::sendCurrentSysexProgram() {
    uint8_t raw[163];

    agentic_dexed::RealtimeSynthState realtime;
    while (!readAgenticRealtimeState(realtime))
        std::this_thread::yield();
    std::array<uint8_t, agentic_dexed::RealtimeSynthState::voiceByteCount> program;
    std::copy(std::begin(data), std::end(data), program.begin());
    std::copy_n(realtime.voiceBytes.begin(), 155, program.begin());
    exportSysexPgm(raw, program.data());
    raw[2] = raw[2] | sysexComm.getChl();
    if ( sysexComm.isOutputActive() ) {
        sysexComm.send(MidiMessage(raw, 163));
    }
}

void DexedAudioProcessor::sendCurrentSysexCartridge() {
    uint8_t raw[4104];

    currentCart.saveVoice(raw);
    raw[2] = raw[2] | sysexComm.getChl();
    if ( sysexComm.isOutputActive() ) {
        sysexComm.send(MidiMessage(raw, 4104));
    }
}

agentic_dexed::ui::UiOperationResult
DexedAudioProcessor::sendSysexCartridge(File cart) {
    if ( ! sysexComm.isOutputActive() )
        return { false, String::fromUTF8("MIDI 输出未连接 / MIDI output is not connected"), false };
    
    std::unique_ptr<juce::FileInputStream> fis = cart.createInputStream();
    if ( fis == NULL )
        return { false, String::fromUTF8("无法打开 SysEx 文件 / Unable to open SysEx file"), false };
    
    uint8 syx_data[65535];
    int sz = fis->read(syx_data, 65535);
    
    if (sz <= 0 || syx_data[0] != 0xF0)
        return { false, String::fromUTF8("文件不包含 SysEx 数据 / File contains no SysEx data"), false };
    sysexComm.send(MidiMessage(syx_data, sz));
    return { true, String::fromUTF8("SysEx 已发送 / SysEx sent"), false };
}

//==============================================================================
void DexedAudioProcessor::getStateInformation(MemoryBlock& destData) {
    // You should use this method to store your parameters in the memory block.
    // You could do that either as raw data, or use the XML or ValueTree classes
    // as intermediaries to make it easy to save and load complex data.
    
    // used to SAVE plugin state
    
    XmlElement dexedState("dexedState");
    XmlElement *dexedBlob = dexedState.createNewChildElement("dexedBlob");

    agentic_dexed::RealtimeSynthState realtime;
    agentic_dexed::SynthSnapshot authoringSnapshot;
    for (;;)
    {
        authoringSnapshot = synthStateService().snapshot(
            { agentic_dexed::SnapshotScopeKind::all, {}, {} });
        if (readAgenticRealtimeState(realtime)
            && realtime.revision == authoringSnapshot.revision)
            break;
        std::this_thread::yield();
    }

    dexedState.setAttribute("agenticStateVersion", 2);
    dexedState.setAttribute("cutoff", realtime.filterCutoff);
    dexedState.setAttribute("reso", realtime.filterResonance);
    dexedState.setAttribute("gain", realtime.outputGain);
    dexedState.setAttribute("currentProgram", currentProgram);
    dexedState.setAttribute("engineType", realtime.engineType);
    const auto tune = static_cast<int32_t>(realtime.hostNormalized[4] * 0x4000) - 0x2000;
    const auto masterTune = static_cast<int32_t>(
        (static_cast<float>(tune * (1 << 11))) * (1.0f / 12.0f));
    dexedState.setAttribute("masterTune", masterTune);

    char opSwitch[7] {};
    for (int index = 0; index < 6; ++index)
        opSwitch[index] = (realtime.voiceBytes[155] & (1u << index)) != 0 ? '1' : '0';
    dexedState.setAttribute("opSwitch", opSwitch);
    dexedState.setAttribute(
        "transpose12AsScale", realtime.performance.transposeAsScale ? 1 : 0);
    dexedState.setAttribute("mpeEnabled", realtime.performance.mpeEnabled ? 1 : 0);
    dexedState.setAttribute(
        "mpePitchBendRange", realtime.performance.mpePitchBendRange);
    dexedState.setAttribute("monoMode", realtime.performance.mono ? 1 : 0);
    const auto portamento = static_cast<int32_t>(
        std::llround(realtime.performance.portamentoTime * 127.0 / 99.0));
    dexedState.setAttribute("portamento", portamento);
    dexedState.setAttribute(
        "glissando", realtime.performance.portamentoGlissando ? 1 : 0);

    const auto modulationConfig = [](const agentic_dexed::RealtimeModulationState& source)
    {
        String result;
        result << source.range << " " << static_cast<int>(source.pitch)
               << " " << static_cast<int>(source.amplitude)
               << " " << static_cast<int>(source.envelope);
        return result;
    };
    dexedState.setAttribute(
        "wheelMod", modulationConfig(realtime.performance.modulation[0]));
    dexedState.setAttribute(
        "footMod", modulationConfig(realtime.performance.modulation[1]));
    dexedState.setAttribute(
        "breathMod", modulationConfig(realtime.performance.modulation[2]));
    dexedState.setAttribute(
        "aftertouchMod", modulationConfig(realtime.performance.modulation[3]));

    const auto& savedSclData = std::get<std::string>(
        authoringSnapshot.values.at("tuning.scl"));
    const auto& savedKbmData = std::get<std::string>(
        authoringSnapshot.values.at("tuning.kbm"));
    if (savedSclData.size() > 1 || savedKbmData.size() > 1)
    {
        auto tuningx = dexedState.createNewChildElement("dexedTuning" );
        auto sclx = tuningx->createNewChildElement("scl");
        sclx->addTextElement(savedSclData);
        auto kbmx = tuningx->createNewChildElement("kbm");
        kbmx->addTextElement(savedKbmData);
    }
    
    NamedValueSet blobSet;
    blobSet.set("sysex", var((void *) currentCart.getVoiceSysex(), 4104));
    std::array<uint8_t, agentic_dexed::RealtimeSynthState::voiceByteCount> program;
    std::copy(std::begin(data), std::end(data), program.begin());
    std::copy_n(realtime.voiceBytes.begin(), 155, program.begin());
    blobSet.set("program", var((void *) program.data(), program.size()));
    
    blobSet.copyToXmlAttributes(*dexedBlob);
    
    XmlElement *midiCC = dexedState.createNewChildElement("midiCC");
    HashMap<int, Ctrl *>::Iterator i(mappedMidiCC);
    while(i.next()) {
        XmlElement *ccMapping = midiCC->createNewChildElement("mapping");
        ccMapping->setAttribute("cc", i.getKey());
        Ctrl *ctrl = i.getValue();
        ccMapping->setAttribute("target", ctrl->label);
    }

    if (agenticAgentController_ != nullptr)
    {
        const auto portable = agenticAgentController_->portableContextSnapshot();
        if (!portable.isEmpty())
        {
            auto* contextNode = dexedState.createNewChildElement("agentContext");
            contextNode->setAttribute("version", agentic_dexed::agent::context::kPortableContextEnvelopeVersion);
            contextNode->setAttribute("encoding", "base64");
            contextNode->setAttribute("sha256",
                agentic_dexed::agent::context::portableContextSha256(portable));
            contextNode->addTextElement(portable.toBase64Encoding());
        }
    }
    
    copyXmlToBinary(dexedState, destData);
}

void DexedAudioProcessor::setStateInformation(const void* source, int sizeInBytes) {
    // You should use this method to restore your parameters from this memory block,
    // whose contents will have been created by the getStateInformation() call.

    // used to LOAD plugin state
    std::unique_ptr<XmlElement> root(getXmlFromBinary(source, sizeInBytes));

    if (root == nullptr) {
        TRACE("unknown state format");
        return;
    }

    if (!root->hasTagName("dexedState")) {
        TRACE("unknown state root");
        return;
    }

    juce::MemoryBlock portableContext;
    bool contextNodePresent = false;
    bool portableContextValid = false;
    if (auto* contextNode = root->getChildByName("agentContext"))
    {
        contextNodePresent = true;
        if (contextNode->getIntAttribute("version")
                == agentic_dexed::agent::context::kPortableContextEnvelopeVersion
            && contextNode->getStringAttribute("encoding") == "base64"
            && contextNode->getAllSubText().length() <= 3 * 1024 * 1024
            && portableContext.fromBase64Encoding(contextNode->getAllSubText())
            && contextNode->getStringAttribute("sha256").toStdString()
                == agentic_dexed::agent::context::portableContextSha256(portableContext)
            && agentic_dexed::agent::context::decodePortableContext(portableContext).ok)
            portableContextValid = true;
    }

    XmlElement *dexedBlob = root->getChildByName("dexedBlob");
    if (dexedBlob == nullptr) {
        TRACE("dexedBlob element not found");
        return;
    }

    NamedValueSet blobSet;
    blobSet.setFromXmlAttributes(*dexedBlob);
    const var sysexBlob = blobSet["sysex"];
    const var programBlob = blobSet["program"];
    if (!sysexBlob.isBinaryData() || !programBlob.isBinaryData()
        || sysexBlob.getBinaryData()->getSize() < 4104
        || programBlob.getBinaryData()->getSize() < 161) {
        TRACE("invalid serialized blob data");
        return;
    }

    Cartridge loadedCart;
    if (loadedCart.load(
            static_cast<const uint8_t*>(sysexBlob.getBinaryData()->getData()),
            static_cast<int>(sysexBlob.getBinaryData()->getSize())) != 0) {
        TRACE("invalid serialized cartridge data");
        return;
    }

    std::string sclData;
    std::string kbmData;
    if (auto* tuningParent = root->getChildByName("dexedTuning"))
    {
        if (auto* scl = tuningParent->getChildByName("scl"))
            if (auto* text = scl->getFirstChildElement();
                text != nullptr && text->isTextElement())
                sclData = text->getText().toStdString();
        if (auto* kbm = tuningParent->getChildByName("kbm"))
            if (auto* text = kbm->getFirstChildElement();
                text != nullptr && text->isTextElement())
                kbmData = text->getText().toStdString();
    }
    if (!agenticTuningDataIsValid(sclData, kbmData)) {
        TRACE("invalid serialized tuning data");
        return;
    }

    const auto validIntegerAttribute = [root = root.get()](
        const char* name, int minimum, int maximum)
    {
        if (!root->hasAttribute(name))
            return true;
        const auto value = root->getIntAttribute(name);
        return value >= minimum && value <= maximum;
    };
    const auto validRealAttribute = [root = root.get()](
        const char* name, double minimum, double maximum)
    {
        if (!root->hasAttribute(name))
            return true;
        const auto value = root->getDoubleAttribute(name);
        return std::isfinite(value) && value >= minimum && value <= maximum;
    };
    const auto opSwitchValue = root->getStringAttribute("opSwitch");
    const auto validOpSwitch = !root->hasAttribute("opSwitch")
        || (opSwitchValue.length() == 6
            && std::all_of(
                opSwitchValue.begin(), opSwitchValue.end(),
                [](const auto character) { return character == '0' || character == '1'; }));
    if (!validRealAttribute("cutoff", 0.0, 1.0)
        || !validRealAttribute("reso", 0.0, 1.0)
        || !validRealAttribute("gain", 0.0, 1.0)
        || !validIntegerAttribute("currentProgram", 0, 31)
        || !validIntegerAttribute("engineType", 0, 2)
        || !validIntegerAttribute("monoMode", 0, 1)
        || !validIntegerAttribute("transpose12AsScale", 0, 1)
        || !validIntegerAttribute("mpeEnabled", 0, 1)
        || !validIntegerAttribute("mpePitchBendRange", 0, 96)
        || !validIntegerAttribute("glissando", 0, 1)
        || !validOpSwitch)
    {
        TRACE("invalid serialized state attribute");
        return;
    }

    auto stateLock = synthStateService().acquireStateLock();

    agentic_dexed::RealtimeSynthState defaults;
    while (!readAgenticRealtimeState(defaults))
        std::this_thread::yield();

    fx.uiCutoff = static_cast<float>(
        root->getDoubleAttribute("cutoff", defaults.filterCutoff));
    fx.uiReso = static_cast<float>(
        root->getDoubleAttribute("reso", defaults.filterResonance));
    fx.uiGain = static_cast<float>(
        root->getDoubleAttribute("gain", defaults.outputGain));
    currentProgram = root->getIntAttribute("currentProgram", currentProgram);

    //TRACE("opSwitch value %s", opSwitchValue.toRawUTF8());
    if ( opSwitchValue.length() != 6 ) {
        for (int index = 0; index < 6; ++index)
            controllers.opSwitch[index] =
                (defaults.voiceBytes[155] & (1u << index)) != 0 ? '1' : '0';
        controllers.opSwitch[6] = '\0';
    } else {
        strncpy(controllers.opSwitch, opSwitchValue.toRawUTF8(), 6);
    }

    const auto modulationConfig = [](const agentic_dexed::RealtimeModulationState& source)
    {
        String result;
        result << source.range << " " << static_cast<int>(source.pitch)
               << " " << static_cast<int>(source.amplitude)
               << " " << static_cast<int>(source.envelope);
        return result;
    };
    controllers.wheel.parseConfig(root->getStringAttribute(
        "wheelMod", modulationConfig(defaults.performance.modulation[0])).toRawUTF8());
    controllers.foot.parseConfig(root->getStringAttribute(
        "footMod", modulationConfig(defaults.performance.modulation[1])).toRawUTF8());
    controllers.breath.parseConfig(root->getStringAttribute(
        "breathMod", modulationConfig(defaults.performance.modulation[2])).toRawUTF8());
    controllers.at.parseConfig(root->getStringAttribute(
        "aftertouchMod", modulationConfig(defaults.performance.modulation[3])).toRawUTF8());

    applyEngineTypeToLegacy(root->getIntAttribute("engineType", defaults.engineType));
    monoMode = root->getIntAttribute("monoMode", defaults.performance.mono ? 1 : 0);
    const auto defaultTune = static_cast<int32_t>(defaults.hostNormalized[4] * 0x4000) - 0x2000;
    const auto defaultMasterTune = static_cast<int32_t>(
        (static_cast<float>(defaultTune * (1 << 11))) * (1.0f / 12.0f));
    controllers.masterTune = root->getIntAttribute("masterTune", defaultMasterTune);
    controllers.transpose12AsScale = (root->getIntAttribute(
        "transpose12AsScale", defaults.performance.transposeAsScale ? 1 : 0) != 0);

    controllers.mpePitchBendRange = root->getIntAttribute(
        "mpePitchBendRange", defaults.performance.mpePitchBendRange);
    controllers.mpeEnabled = (root->getIntAttribute(
        "mpeEnabled", defaults.performance.mpeEnabled ? 1 : 0) != 0);

    const auto defaultPortamento = static_cast<int32_t>(
        std::llround(defaults.performance.portamentoTime * 127.0 / 99.0));
    const auto serializedPortamento = root->getIntAttribute(
        "portamento", defaultPortamento);
    controllers.portamento_cc = serializedPortamento >= 0 && serializedPortamento <= 127
        ? serializedPortamento : defaultPortamento;
    controllers.portamento_enable_cc = controllers.portamento_cc > 1;
    controllers.portamento_gliss_cc = (root->getIntAttribute(
        "glissando", defaults.performance.portamentoGlissando ? 1 : 0) != 0);
    controllers.refresh();

    File possibleCartridge = File(root->getStringAttribute("activeFileCartridge"));
    if ( possibleCartridge.exists() )
        activeFileCartridge = possibleCartridge;

    setAgenticTuningData(sclData, kbmData);
    loadCartridge(loadedCart);
    memcpy(data, programBlob.getBinaryData()->getData(), 161);
    
    mappedMidiCC.clear();
    XmlElement *midiCC = root->getChildByName("midiCC");
    if ( midiCC != nullptr ) {
        XmlElement *ccMapping = midiCC->getFirstChildElement();
        while (ccMapping != nullptr) {
            int cc = ccMapping->getIntAttribute("cc", -1);
            String target = ccMapping->getStringAttribute("target", "");
            if ( target.isNotEmpty() && cc != -1 ) {
                for(int i=0;i<ctrl.size();i++) {
                    if ((cc >> 8) == 0) {
                        // Simple migration logic lets old mappings without channel
                        // work on channel 1.
                        cc |= 1 << 8;
                    }
                    if ( ctrl[i]->label == target) {
                        TRACE("mapping CC=%d to %s", cc, target.toRawUTF8());
                        mappedMidiCC.set(cc, ctrl[i]);
                        break;
                    }
                }
            }
            ccMapping = ccMapping->getNextElement();
        }
    }
    
    lastStateSave = (long) time(NULL);
    TRACE("setting VST STATE");
    publishLegacyStateToRealtimeStore();
    panic();
    stateLock.unlock();
    updateUI();
    if (agenticAgentController_ != nullptr)
    {
        bool imported = true;
        if (portableContextValid)
            imported = agenticAgentController_->importPortableContext(portableContext);
        else
            agenticAgentController_->resetPortableContext();
        lastAgentContextImportOk_.store(
            !contextNodePresent || (portableContextValid && imported),
            std::memory_order_release);
    }
}

File DexedAudioProcessor::dexedAppDir;
File DexedAudioProcessor::dexedCartDir;

void DexedAudioProcessor::resolvAppDir() {
    #if JUCE_MAC || JUCE_IOS
        File parent = File::getSpecialLocation(File::currentExecutableFile).getParentDirectory().getParentDirectory().getParentDirectory().getSiblingFile("Dexed");
    
        if ( parent.isDirectory() ) {
            dexedAppDir = parent;
        } else {
            dexedAppDir = File("~/Library/Application Support/DigitalSuburban/Dexed");
        }
    #elif JUCE_WINDOWS
        if ( File::getSpecialLocation(File::currentExecutableFile).getSiblingFile("Dexed").isDirectory() ) {
            dexedAppDir = File::getSpecialLocation(File::currentExecutableFile).getSiblingFile("Dexed");
        } else {
            dexedAppDir = File::getSpecialLocation(File::userApplicationDataDirectory).getChildFile("DigitalSuburban").getChildFile("Dexed");
        }
    #else
        if ( File::getSpecialLocation(File::currentExecutableFile).getSiblingFile("Dexed").isDirectory() ) {
            dexedAppDir = File::getSpecialLocation(File::currentExecutableFile).getSiblingFile("Dexed");
        } else {
            char *xdgHome = getenv("XDG_DATA_HOME");
            if ( xdgHome == nullptr ) {
                dexedAppDir = File("~/.local/share").getChildFile("DigitalSuburban").getChildFile("Dexed");
            } else {
                dexedAppDir = File(xdgHome).getChildFile("DigitalSuburban").getChildFile("Dexed");
            }
        }
    #endif
    
    if ( ! dexedAppDir.exists() ) {
        dexedAppDir.createDirectory();
        // ==========================================================================
        // For older versions, we move the Dexed.xml config file
        // This code will be removed in 0.9.0
        File cfgFile = dexedAppDir.getParentDirectory().getChildFile("Dexed.xml");
        if ( cfgFile.exists() )
            cfgFile.moveFileTo(dexedAppDir.getChildFile("Dexed.xml"));
        // *-*-*-*-*-*-*-*-*-*-*-*-*-*-*-*-*-*-*-*-*-*-*-*-*-*-*-*-*-*-*-*-*-*-*-*-*-
        // ==========================================================================
    }
    
    dexedCartDir = dexedAppDir.getChildFile("Cartridges");

    if ( ! dexedCartDir.exists() ) {
        // Initial setup, we unzip the built-in cartridges
        dexedCartDir.createDirectory();
        File synprezFmDir = dexedCartDir.getChildFile("SynprezFM");
        synprezFmDir.createDirectory();
        
        MemoryInputStream *mis = new MemoryInputStream(BinaryData::builtin_pgm_zip, BinaryData::builtin_pgm_zipSize, false);
        ZipFile *builtin_pgm = new ZipFile(mis, true);
        
        for(int i=0;i<builtin_pgm->getNumEntries();i++) {
            if ( builtin_pgm->getEntry(i)->filename == "Dexed_01.syx" ) {
                builtin_pgm->uncompressEntry(i, dexedCartDir);
            } else {
                builtin_pgm->uncompressEntry(i, synprezFmDir);
            }
        }
        delete builtin_pgm;
    }
}
