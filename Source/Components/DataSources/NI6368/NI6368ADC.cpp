/**
 * @file NI6368ADC.cpp
 * @brief Source file for class NI6368ADC
 * @date 12/01/2017
 * @author Andre Neto
 *
 * @copyright Copyright 2015 F4E | European Joint Undertaking for ITER and
 * the Development of Fusion Energy ('Fusion for Energy').
 * Licensed under the EUPL, Version 1.1 or - as soon they will be approved
 * by the European Commission - subsequent versions of the EUPL (the "Licence")
 * You may not use this work except in compliance with the Licence.
 * You may obtain a copy of the Licence at: http://ec.europa.eu/idabc/eupl
 *
 * @warning Unless required by applicable law or agreed to in writing, 
 * software distributed under the Licence is distributed on an "AS IS"
 * basis, WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express
 * or implied. See the Licence permissions and limitations under the Licence.

 * @details This source file contains the definition of all the methods for
 * the class NI6368ADC (public, protected, and private). Be aware that some
 * methods, such as those inline could be defined on the header file, instead.
 */

/*---------------------------------------------------------------------------*/
/*                         Standard header includes                          */
/*---------------------------------------------------------------------------*/
#include "NI6368ADC.h"

#include <fcntl.h>

/*---------------------------------------------------------------------------*/
/*                         Project header includes                           */
/*---------------------------------------------------------------------------*/
#include "AdvancedErrorManagement.h"
#include "MemoryMapInputBroker.h"
#include "MemoryMapSynchronisedInputBroker.h"

/*---------------------------------------------------------------------------*/
/*                           Static definitions                              */
/*---------------------------------------------------------------------------*/

/*---------------------------------------------------------------------------*/
/*                           Method definitions                              */
/*---------------------------------------------------------------------------*/

namespace MARTe {
NI6368ADC::NI6368ADC() :
        DataSourceI(), EmbeddedServiceMethodBinderI(), executor(*this) {
    cycleFrequency = 0.F;
    numberOfSamples = 0u;
    boardId = 0u;
    boardFileDescriptor = -1;
    deviceName = "";
    counter = 0u;
    timeValue = 0u;
    scanIntervalCounterDelay = 0u;
    scanIntervalCounterPeriod = 0u;
    numberOfADCsEnabled = 0u;
    clockSampleSource = XSERIES_AI_SAMPLE_CONVERT_CLOCK_INTERNALTIMING;
    clockSamplePolarity = XSERIES_AI_POLARITY_ACTIVE_HIGH_OR_RISING_EDGE;
    clockConvertSource = XSERIES_AI_SAMPLE_CONVERT_CLOCK_INTERNALTIMING;
    clockConvertPolarity = XSERIES_AI_POLARITY_ACTIVE_HIGH_OR_RISING_EDGE;
    scanIntervalCounterSource = XSERIES_SCAN_INTERVAL_COUNTER_TB3;
    scanIntervalCounterPolarity = XSERIES_SCAN_INTERVAL_COUNTER_POLARITY_RISING_EDGE;

    keepRunning = true;
    synchronising = false;
    cpuMask = 0u;
    uint32 n;
    for (n = 0u; n < NI6368ADC_MAX_CHANNELS; n++) {
        inputRange[n] = XSERIES_INPUT_RANGE_10V;
        inputType[n] = XSERIES_AI_CHANNEL_TYPE_RSE;
        adcEnabled[n] = false;
        channelsFileDescriptors[n] = -1;
        channelsMemory[n] = NULL_PTR(float32 *);
    }
    channelMemory = NULL_PTR(float32 *);
    if (!synchSem.Create()) {
        REPORT_ERROR(ErrorManagement::FatalError, "Could not create EventSem.");
    }
}

/*lint -e{1551} the destructor must guarantee that the NI6368ADC SingleThreadService is stopped and that all the file descriptors are closed.*/
NI6368ADC::~NI6368ADC() {
    if (!executor.Stop()) {
        if (!executor.Stop()) {
            REPORT_ERROR(ErrorManagement::FatalError, "Could not stop SingleThreadService.");
        }
    }
    uint32 n;
    if (boardFileDescriptor != -1) {
        if (xseries_stop_ai(boardFileDescriptor) < 0) {
            REPORT_ERROR(ErrorManagement::FatalError, "Could not stop acquisition.");
        }
    }
    for (n = 0u; n < NI6368ADC_MAX_CHANNELS; n++) {
        if (channelsFileDescriptors[n] != -1) {
            close(channelsFileDescriptors[n]);
        }
    }
    if (boardFileDescriptor != -1) {
        close(boardFileDescriptor);
    }
    for (n = 0u; n < NI6368ADC_MAX_CHANNELS; n++) {
        if (channelsMemory[n] != NULL_PTR(float32 *)) {
            delete[] channelsMemory[n];
        }
    }
    if (channelMemory != NULL_PTR(float32 *)) {
        delete[] channelMemory;
    }
}

bool NI6368ADC::AllocateMemory() {
    return true;
}

uint32 NI6368ADC::GetNumberOfMemoryBuffers() {
    return 1u;
}

/*lint -e{715}  [MISRA C++ Rule 0-1-11], [MISRA C++ Rule 0-1-12]. Justification: The memory buffer is independent of the bufferIdx.*/
bool NI6368ADC::GetSignalMemoryBuffer(const uint32 signalIdx, const uint32 bufferIdx, void*& signalAddress) {
    bool ok = (signalIdx < (NI6368ADC_MAX_CHANNELS + NI6368ADC_HEADER_SIZE));
    if (ok) {
        if (signalIdx == 0u) {
            signalAddress = reinterpret_cast<void *>(&counter);
        }
        else if (signalIdx == 1u) {
            signalAddress = reinterpret_cast<void *>(&timeValue);
        }
        else {
            signalAddress = &(channelsMemory[signalIdx - NI6368ADC_HEADER_SIZE][0]);
        }
    }
    return ok;
}

const char8* NI6368ADC::GetBrokerName(StructuredDataI& data, const SignalDirection direction) {
    const char8 *brokerName = NULL_PTR(const char8 *);
    if (direction == InputSignals) {
        float32 frequency = 0.F;
        if (!data.Read("Frequency", frequency)) {
            frequency = -1.F;
        }

        if (frequency > 0.F) {
            brokerName = "MemoryMapSynchronisedInputBroker";
            cycleFrequency = frequency;
            synchronising = true;
        }
        else {
            brokerName = "MemoryMapInputBroker";
        }
    }
    else {
        REPORT_ERROR(ErrorManagement::ParametersError, "DataSource not compatible with OutputSignals");
    }
    return brokerName;
}

bool NI6368ADC::GetInputBrokers(ReferenceContainer& inputBrokers, const char8* const functionName, void* const gamMemPtr) {
    //Check if this function has a synchronisation point (i.e. a signal which has Frequency > 0)
    uint32 functionIdx = 0u;
    uint32 nOfSignals = 0u;

    bool synchGAM = false;
    bool ok = GetFunctionIndex(functionIdx, functionName);
    if (ok) {
        ok = GetFunctionNumberOfSignals(InputSignals, functionIdx, nOfSignals);
    }

    uint32 i;
    float32 frequency = 0.F;
    for (i = 0u; (i < nOfSignals) && (ok) && (!synchGAM); i++) {
        ok = GetFunctionSignalReadFrequency(InputSignals, functionIdx, i, frequency);
        synchGAM = (frequency > 0.F);
    }
    if ((synchronising) && (synchGAM)) {
        ReferenceT<MemoryMapSynchronisedInputBroker> brokerSync("MemoryMapSynchronisedInputBroker");
        if (ok) {
            ok = brokerSync.IsValid();
        }
        if (ok) {
            ok = brokerSync->Init(InputSignals, *this, functionName, gamMemPtr);
        }
        if (ok) {
            ok = inputBrokers.Insert(brokerSync);
        }
        uint32 nOfFunctionSignals = 0u;
        if (ok) {
            ok = GetFunctionNumberOfSignals(InputSignals, functionIdx, nOfFunctionSignals);
        }
        //Must also add the signals which are not synchronous but that belong to the same GAM...
        if (ok) {
            if (nOfFunctionSignals > 1u) {
                ReferenceT<MemoryMapInputBroker> brokerNotSync("MemoryMapInputBroker");
                ok = brokerNotSync.IsValid();
                if (ok) {
                    ok = brokerNotSync->Init(InputSignals, *this, functionName, gamMemPtr);
                }
                if (ok) {
                    ok = inputBrokers.Insert(brokerNotSync);
                }
            }
        }
    }
    else {
        ReferenceT<MemoryMapInputBroker> broker("MemoryMapInputBroker");
        ok = broker.IsValid();
        if (ok) {
            ok = broker->Init(InputSignals, *this, functionName, gamMemPtr);
        }
        if (ok) {
            ok = inputBrokers.Insert(broker);
        }
    }

    return ok;
}

/*lint -e{715}  [MISRA C++ Rule 0-1-11], [MISRA C++ Rule 0-1-12]. Justification: returns false irrespectively of the input parameters.*/
bool NI6368ADC::GetOutputBrokers(ReferenceContainer& outputBrokers, const char8* const functionName, void* const gamMemPtr) {
    return false;
}

bool NI6368ADC::Synchronise() {
    ErrorManagement::ErrorType err;
    if (synchronising) {
        err = synchSem.ResetWait(TTInfiniteWait);
    }
    return err.ErrorsCleared();
}

/*lint -e{715}  [MISRA C++ Rule 0-1-11], [MISRA C++ Rule 0-1-12]. Justification: the counter and the timer are always reset irrespectively of the states being changed.*/
bool NI6368ADC::PrepareNextState(const char8* const currentStateName, const char8* const nextStateName) {
    counter = 0u;
    timeValue = 0u;
    bool ok = true;
    if (executor.GetStatus() == EmbeddedThreadI::OffState) {
        keepRunning = true;
        if (cpuMask != 0u) {
            executor.SetCPUMask(cpuMask);
        }
        ok = executor.Start();
    }
    return ok;
}

bool NI6368ADC::Initialise(StructuredDataI& data) {
    bool ok = DataSourceI::Initialise(data);
    if (ok) {
        ok = data.Read("DeviceName", deviceName);
        if (!ok) {
            REPORT_ERROR(ErrorManagement::ParametersError, "The DeviceName shall be specified");
        }
    }
    if (ok) {
        ok = data.Read("BoardId", boardId);
        if (!ok) {
            REPORT_ERROR(ErrorManagement::ParametersError, "The BoardId shall be specified");
        }
    }
    if (ok) {
        ok = data.Read("ScanIntervalCounterPeriod", scanIntervalCounterPeriod);
        if (!ok) {
            REPORT_ERROR(ErrorManagement::ParametersError, "The ScanIntervalCounterPeriod shall be specified");
        }
    }
    if (ok) {
        ok = data.Read("ScanIntervalCounterDelay", scanIntervalCounterDelay);
        if (!ok) {
            REPORT_ERROR(ErrorManagement::ParametersError, "The ScanIntervalCounterDelay shall be specified");
        }
    }
    StreamString clockSampleSourceStr;
    if (ok) {
        ok = data.Read("ClockSampleSource", clockSampleSourceStr);
        if (!ok) {
            REPORT_ERROR(ErrorManagement::ParametersError, "The ClockSampleSource shall be specified");
        }
    }
    if (ok) {
        if (clockSampleSourceStr == "INTERNALTIMING") {
            clockSampleSource = XSERIES_AI_SAMPLE_CONVERT_CLOCK_INTERNALTIMING;
        }
        else if (clockSampleSourceStr == "PFI0") {
            clockSampleSource = XSERIES_AI_SAMPLE_CONVERT_CLOCK_PFI0;
        }
        else if (clockSampleSourceStr == "PFI1") {
            clockSampleSource = XSERIES_AI_SAMPLE_CONVERT_CLOCK_PFI1;
        }
        else if (clockSampleSourceStr == "PFI2") {
            clockSampleSource = XSERIES_AI_SAMPLE_CONVERT_CLOCK_PFI2;
        }
        else if (clockSampleSourceStr == "PFI3") {
            clockSampleSource = XSERIES_AI_SAMPLE_CONVERT_CLOCK_PFI3;
        }
        else if (clockSampleSourceStr == "PFI4") {
            clockSampleSource = XSERIES_AI_SAMPLE_CONVERT_CLOCK_PFI4;
        }
        else if (clockSampleSourceStr == "PFI5") {
            clockSampleSource = XSERIES_AI_SAMPLE_CONVERT_CLOCK_PFI5;
        }
        else if (clockSampleSourceStr == "PFI6") {
            clockSampleSource = XSERIES_AI_SAMPLE_CONVERT_CLOCK_PFI6;
        }
        else if (clockSampleSourceStr == "PFI7") {
            clockSampleSource = XSERIES_AI_SAMPLE_CONVERT_CLOCK_PFI7;
        }
        else if (clockSampleSourceStr == "PFI8") {
            clockSampleSource = XSERIES_AI_SAMPLE_CONVERT_CLOCK_PFI8;
        }
        else if (clockSampleSourceStr == "PFI9") {
            clockSampleSource = XSERIES_AI_SAMPLE_CONVERT_CLOCK_PFI9;
        }
        else if (clockSampleSourceStr == "PFI10") {
            clockSampleSource = XSERIES_AI_SAMPLE_CONVERT_CLOCK_PFI10;
        }
        else if (clockSampleSourceStr == "PFI11") {
            clockSampleSource = XSERIES_AI_SAMPLE_CONVERT_CLOCK_PFI11;
        }
        else if (clockSampleSourceStr == "PFI12") {
            clockSampleSource = XSERIES_AI_SAMPLE_CONVERT_CLOCK_PFI12;
        }
        else if (clockSampleSourceStr == "PFI13") {
            clockSampleSource = XSERIES_AI_SAMPLE_CONVERT_CLOCK_PFI13;
        }
        else if (clockSampleSourceStr == "PFI14") {
            clockSampleSource = XSERIES_AI_SAMPLE_CONVERT_CLOCK_PFI14;
        }
        else if (clockSampleSourceStr == "PFI15") {
            clockSampleSource = XSERIES_AI_SAMPLE_CONVERT_CLOCK_PFI15;
        }
        else if (clockSampleSourceStr == "RTSI0") {
            clockSampleSource = XSERIES_AI_SAMPLE_CONVERT_CLOCK_RTSI0;
        }
        else if (clockSampleSourceStr == "RTSI1") {
            clockSampleSource = XSERIES_AI_SAMPLE_CONVERT_CLOCK_RTSI1;
        }
        else if (clockSampleSourceStr == "RTSI2") {
            clockSampleSource = XSERIES_AI_SAMPLE_CONVERT_CLOCK_RTSI2;
        }
        else if (clockSampleSourceStr == "RTSI3") {
            clockSampleSource = XSERIES_AI_SAMPLE_CONVERT_CLOCK_RTSI3;
        }
        else if (clockSampleSourceStr == "RTSI4") {
            clockSampleSource = XSERIES_AI_SAMPLE_CONVERT_CLOCK_RTSI4;
        }
        else if (clockSampleSourceStr == "RTSI5") {
            clockSampleSource = XSERIES_AI_SAMPLE_CONVERT_CLOCK_RTSI5;
        }
        else if (clockSampleSourceStr == "RTSI6") {
            clockSampleSource = XSERIES_AI_SAMPLE_CONVERT_CLOCK_RTSI6;
        }
        else if (clockSampleSourceStr == "RTSI7") {
            clockSampleSource = XSERIES_AI_SAMPLE_CONVERT_CLOCK_RTSI7;
        }
        else if (clockSampleSourceStr == "DIO_CHGDETECT") {
            clockSampleSource = XSERIES_AI_SAMPLE_CONVERT_CLOCK_DIO_CHGDETECT;
        }
        else if (clockSampleSourceStr == "G0_OUT") {
            clockSampleSource = XSERIES_AI_SAMPLE_CONVERT_CLOCK_G0_OUT;
        }
        else if (clockSampleSourceStr == "G1_OUT") {
            clockSampleSource = XSERIES_AI_SAMPLE_CONVERT_CLOCK_G1_OUT;
        }
        else if (clockSampleSourceStr == "G2_OUT") {
            clockSampleSource = XSERIES_AI_SAMPLE_CONVERT_CLOCK_G2_OUT;
        }
        else if (clockSampleSourceStr == "G3_OUT") {
            clockSampleSource = XSERIES_AI_SAMPLE_CONVERT_CLOCK_G3_OUT;
        }
        else if (clockSampleSourceStr == "STAR_TRIGGER") {
            clockSampleSource = XSERIES_AI_SAMPLE_CONVERT_CLOCK_STAR_TRIGGER;
        }
        else if (clockSampleSourceStr == "SCXI_TRIG1") {
            clockSampleSource = XSERIES_AI_SAMPLE_CONVERT_CLOCK_SCXI_TRIG1;
        }
        else if (clockSampleSourceStr == "ATRIG") {
            clockSampleSource = XSERIES_AI_SAMPLE_CONVERT_CLOCK_ATRIG;
        }
        else if (clockSampleSourceStr == "LOW") {
            clockSampleSource = XSERIES_AI_SAMPLE_CONVERT_CLOCK_LOW;
        }
        else if (clockSampleSourceStr == "PXIE_DSTARA") {
            clockSampleSource = XSERIES_AI_SAMPLE_CONVERT_CLOCK_PXIE_DSTARA;
        }
        else if (clockSampleSourceStr == "PXIE_DSTARB") {
            clockSampleSource = XSERIES_AI_SAMPLE_CONVERT_CLOCK_PXIE_DSTARB;
        }
        else if (clockSampleSourceStr == "G0_SAMPLECLK") {
            clockSampleSource = XSERIES_AI_SAMPLE_CONVERT_CLOCK_G0_SAMPLECLK;
        }
        else if (clockSampleSourceStr == "G1_SAMPLECLK") {
            clockSampleSource = XSERIES_AI_SAMPLE_CONVERT_CLOCK_G1_SAMPLECLK;
        }
        else if (clockSampleSourceStr == "G2_SAMPLECLK") {
            clockSampleSource = XSERIES_AI_SAMPLE_CONVERT_CLOCK_G2_SAMPLECLK;
        }
        else if (clockSampleSourceStr == "G3_SAMPLECLK") {
            clockSampleSource = XSERIES_AI_SAMPLE_CONVERT_CLOCK_G3_SAMPLECLK;
        }
        else if (clockSampleSourceStr == "DI_CONVERT") {
            clockSampleSource = XSERIES_AI_SAMPLE_CONVERT_CLOCK_DI_CONVERT;
        }
        else if (clockSampleSourceStr == "AO_UPDATE") {
            clockSampleSource = XSERIES_AI_SAMPLE_CONVERT_CLOCK_AO_UPDATE;
        }
        else if (clockSampleSourceStr == "DO_UPDATE") {
            clockSampleSource = XSERIES_AI_SAMPLE_CONVERT_CLOCK_DO_UPDATE;
        }
        else if (clockSampleSourceStr == "INTTRIGGERA0") {
            clockSampleSource = XSERIES_AI_SAMPLE_CONVERT_CLOCK_INTTRIGGERA0;
        }
        else if (clockSampleSourceStr == "INTTRIGGERA1") {
            clockSampleSource = XSERIES_AI_SAMPLE_CONVERT_CLOCK_INTTRIGGERA1;
        }
        else if (clockSampleSourceStr == "INTTRIGGERA2") {
            clockSampleSource = XSERIES_AI_SAMPLE_CONVERT_CLOCK_INTTRIGGERA2;
        }
        else if (clockSampleSourceStr == "INTTRIGGERA3") {
            clockSampleSource = XSERIES_AI_SAMPLE_CONVERT_CLOCK_INTTRIGGERA3;
        }
        else if (clockSampleSourceStr == "INTTRIGGERA4") {
            clockSampleSource = XSERIES_AI_SAMPLE_CONVERT_CLOCK_INTTRIGGERA4;
        }
        else if (clockSampleSourceStr == "INTTRIGGERA5") {
            clockSampleSource = XSERIES_AI_SAMPLE_CONVERT_CLOCK_INTTRIGGERA5;
        }
        else if (clockSampleSourceStr == "INTTRIGGERA6") {
            clockSampleSource = XSERIES_AI_SAMPLE_CONVERT_CLOCK_INTTRIGGERA6;
        }
        else if (clockSampleSourceStr == "INTTRIGGERA7") {
            clockSampleSource = XSERIES_AI_SAMPLE_CONVERT_CLOCK_INTTRIGGERA7;
        }
        else {
            ok = false;
            REPORT_ERROR(ErrorManagement::ParametersError, "Unsupported ClockSampleSource");
        }
    }
    StreamString clockSamplePolarityStr;
    if (ok) {
        ok = data.Read("ClockSamplePolarity", clockSamplePolarityStr);
        if (!ok) {
            REPORT_ERROR(ErrorManagement::ParametersError, "The ClockSamplePolarity shall be specified");
        }
    }
    if (ok) {
        if (clockSamplePolarityStr == "ACTIVE_HIGH_OR_RISING_EDGE") {
            clockSamplePolarity = XSERIES_AI_POLARITY_ACTIVE_HIGH_OR_RISING_EDGE;
        }
        else if (clockSamplePolarityStr == "ACTIVE_LOW_OR_FALLING_EDGE") {
            clockSamplePolarity = XSERIES_AI_POLARITY_ACTIVE_LOW_OR_FALLING_EDGE;
        }
        else {
            ok = false;
            REPORT_ERROR(ErrorManagement::ParametersError, "Unsupported ClockSamplePolarity");
        }
    }
    StreamString clockConvertSourceStr;
    if (ok) {
        ok = data.Read("ClockConvertSource", clockConvertSourceStr);
        if (!ok) {
            REPORT_ERROR(ErrorManagement::ParametersError, "The ClockConvertSource shall be specified");
        }
    }
    if (ok) {
        if (clockConvertSourceStr == "INTERNALTIMING") {
            clockConvertSource = XSERIES_AI_SAMPLE_CONVERT_CLOCK_INTERNALTIMING;
        }
        else if (clockConvertSourceStr == "PFI0") {
            clockConvertSource = XSERIES_AI_SAMPLE_CONVERT_CLOCK_PFI0;
        }
        else if (clockConvertSourceStr == "PFI1") {
            clockConvertSource = XSERIES_AI_SAMPLE_CONVERT_CLOCK_PFI1;
        }
        else if (clockConvertSourceStr == "PFI2") {
            clockConvertSource = XSERIES_AI_SAMPLE_CONVERT_CLOCK_PFI2;
        }
        else if (clockConvertSourceStr == "PFI3") {
            clockConvertSource = XSERIES_AI_SAMPLE_CONVERT_CLOCK_PFI3;
        }
        else if (clockConvertSourceStr == "PFI4") {
            clockConvertSource = XSERIES_AI_SAMPLE_CONVERT_CLOCK_PFI4;
        }
        else if (clockConvertSourceStr == "PFI5") {
            clockConvertSource = XSERIES_AI_SAMPLE_CONVERT_CLOCK_PFI5;
        }
        else if (clockConvertSourceStr == "PFI6") {
            clockConvertSource = XSERIES_AI_SAMPLE_CONVERT_CLOCK_PFI6;
        }
        else if (clockConvertSourceStr == "PFI7") {
            clockConvertSource = XSERIES_AI_SAMPLE_CONVERT_CLOCK_PFI7;
        }
        else if (clockConvertSourceStr == "PFI8") {
            clockConvertSource = XSERIES_AI_SAMPLE_CONVERT_CLOCK_PFI8;
        }
        else if (clockConvertSourceStr == "PFI9") {
            clockConvertSource = XSERIES_AI_SAMPLE_CONVERT_CLOCK_PFI9;
        }
        else if (clockConvertSourceStr == "PFI10") {
            clockConvertSource = XSERIES_AI_SAMPLE_CONVERT_CLOCK_PFI10;
        }
        else if (clockConvertSourceStr == "PFI11") {
            clockConvertSource = XSERIES_AI_SAMPLE_CONVERT_CLOCK_PFI11;
        }
        else if (clockConvertSourceStr == "PFI12") {
            clockConvertSource = XSERIES_AI_SAMPLE_CONVERT_CLOCK_PFI12;
        }
        else if (clockConvertSourceStr == "PFI13") {
            clockConvertSource = XSERIES_AI_SAMPLE_CONVERT_CLOCK_PFI13;
        }
        else if (clockConvertSourceStr == "PFI14") {
            clockConvertSource = XSERIES_AI_SAMPLE_CONVERT_CLOCK_PFI14;
        }
        else if (clockConvertSourceStr == "PFI15") {
            clockConvertSource = XSERIES_AI_SAMPLE_CONVERT_CLOCK_PFI15;
        }
        else if (clockConvertSourceStr == "RTSI0") {
            clockConvertSource = XSERIES_AI_SAMPLE_CONVERT_CLOCK_RTSI0;
        }
        else if (clockConvertSourceStr == "RTSI1") {
            clockConvertSource = XSERIES_AI_SAMPLE_CONVERT_CLOCK_RTSI1;
        }
        else if (clockConvertSourceStr == "RTSI2") {
            clockConvertSource = XSERIES_AI_SAMPLE_CONVERT_CLOCK_RTSI2;
        }
        else if (clockConvertSourceStr == "RTSI3") {
            clockConvertSource = XSERIES_AI_SAMPLE_CONVERT_CLOCK_RTSI3;
        }
        else if (clockConvertSourceStr == "RTSI4") {
            clockConvertSource = XSERIES_AI_SAMPLE_CONVERT_CLOCK_RTSI4;
        }
        else if (clockConvertSourceStr == "RTSI5") {
            clockConvertSource = XSERIES_AI_SAMPLE_CONVERT_CLOCK_RTSI5;
        }
        else if (clockConvertSourceStr == "RTSI6") {
            clockConvertSource = XSERIES_AI_SAMPLE_CONVERT_CLOCK_RTSI6;
        }
        else if (clockConvertSourceStr == "RTSI7") {
            clockConvertSource = XSERIES_AI_SAMPLE_CONVERT_CLOCK_RTSI7;
        }
        else if (clockConvertSourceStr == "DIO_CHGDETECT") {
            clockConvertSource = XSERIES_AI_SAMPLE_CONVERT_CLOCK_DIO_CHGDETECT;
        }
        else if (clockConvertSourceStr == "G0_OUT") {
            clockConvertSource = XSERIES_AI_SAMPLE_CONVERT_CLOCK_G0_OUT;
        }
        else if (clockConvertSourceStr == "G1_OUT") {
            clockConvertSource = XSERIES_AI_SAMPLE_CONVERT_CLOCK_G1_OUT;
        }
        else if (clockConvertSourceStr == "G2_OUT") {
            clockConvertSource = XSERIES_AI_SAMPLE_CONVERT_CLOCK_G2_OUT;
        }
        else if (clockConvertSourceStr == "G3_OUT") {
            clockConvertSource = XSERIES_AI_SAMPLE_CONVERT_CLOCK_G3_OUT;
        }
        else if (clockConvertSourceStr == "STAR_TRIGGER") {
            clockConvertSource = XSERIES_AI_SAMPLE_CONVERT_CLOCK_STAR_TRIGGER;
        }
        else if (clockConvertSourceStr == "SCXI_TRIG1") {
            clockConvertSource = XSERIES_AI_SAMPLE_CONVERT_CLOCK_SCXI_TRIG1;
        }
        else if (clockConvertSourceStr == "ATRIG") {
            clockConvertSource = XSERIES_AI_SAMPLE_CONVERT_CLOCK_ATRIG;
        }
        else if (clockConvertSourceStr == "LOW") {
            clockConvertSource = XSERIES_AI_SAMPLE_CONVERT_CLOCK_LOW;
        }
        else if (clockConvertSourceStr == "PXIE_DSTARA") {
            clockConvertSource = XSERIES_AI_SAMPLE_CONVERT_CLOCK_PXIE_DSTARA;
        }
        else if (clockConvertSourceStr == "PXIE_DSTARB") {
            clockConvertSource = XSERIES_AI_SAMPLE_CONVERT_CLOCK_PXIE_DSTARB;
        }
        else if (clockConvertSourceStr == "G0_SAMPLECLK") {
            clockConvertSource = XSERIES_AI_SAMPLE_CONVERT_CLOCK_G0_SAMPLECLK;
        }
        else if (clockConvertSourceStr == "G1_SAMPLECLK") {
            clockConvertSource = XSERIES_AI_SAMPLE_CONVERT_CLOCK_G1_SAMPLECLK;
        }
        else if (clockConvertSourceStr == "G2_SAMPLECLK") {
            clockConvertSource = XSERIES_AI_SAMPLE_CONVERT_CLOCK_G2_SAMPLECLK;
        }
        else if (clockConvertSourceStr == "G3_SAMPLECLK") {
            clockConvertSource = XSERIES_AI_SAMPLE_CONVERT_CLOCK_G3_SAMPLECLK;
        }
        else if (clockConvertSourceStr == "DI_CONVERT") {
            clockConvertSource = XSERIES_AI_SAMPLE_CONVERT_CLOCK_DI_CONVERT;
        }
        else if (clockConvertSourceStr == "AO_UPDATE") {
            clockConvertSource = XSERIES_AI_SAMPLE_CONVERT_CLOCK_AO_UPDATE;
        }
        else if (clockConvertSourceStr == "DO_UPDATE") {
            clockConvertSource = XSERIES_AI_SAMPLE_CONVERT_CLOCK_DO_UPDATE;
        }
        else if (clockConvertSourceStr == "INTTRIGGERA0") {
            clockConvertSource = XSERIES_AI_SAMPLE_CONVERT_CLOCK_INTTRIGGERA0;
        }
        else if (clockConvertSourceStr == "INTTRIGGERA1") {
            clockConvertSource = XSERIES_AI_SAMPLE_CONVERT_CLOCK_INTTRIGGERA1;
        }
        else if (clockConvertSourceStr == "INTTRIGGERA2") {
            clockConvertSource = XSERIES_AI_SAMPLE_CONVERT_CLOCK_INTTRIGGERA2;
        }
        else if (clockConvertSourceStr == "INTTRIGGERA3") {
            clockConvertSource = XSERIES_AI_SAMPLE_CONVERT_CLOCK_INTTRIGGERA3;
        }
        else if (clockConvertSourceStr == "INTTRIGGERA4") {
            clockConvertSource = XSERIES_AI_SAMPLE_CONVERT_CLOCK_INTTRIGGERA4;
        }
        else if (clockConvertSourceStr == "INTTRIGGERA5") {
            clockConvertSource = XSERIES_AI_SAMPLE_CONVERT_CLOCK_INTTRIGGERA5;
        }
        else if (clockConvertSourceStr == "INTTRIGGERA6") {
            clockConvertSource = XSERIES_AI_SAMPLE_CONVERT_CLOCK_INTTRIGGERA6;
        }
        else if (clockConvertSourceStr == "INTTRIGGERA7") {
            clockConvertSource = XSERIES_AI_SAMPLE_CONVERT_CLOCK_INTTRIGGERA7;
        }
        else {
            ok = false;
            REPORT_ERROR(ErrorManagement::ParametersError, "Unsupported ClockConvertSource");
        }
    }
    StreamString clockConvertPolarityStr;
    if (ok) {
        ok = data.Read("ClockConvertPolarity", clockConvertPolarityStr);
        if (!ok) {
            REPORT_ERROR(ErrorManagement::ParametersError, "The ClockConvertPolarity shall be specified");
        }
    }
    if (ok) {
        if (clockConvertPolarityStr == "ACTIVE_HIGH_OR_RISING_EDGE") {
            clockConvertPolarity = XSERIES_AI_POLARITY_ACTIVE_HIGH_OR_RISING_EDGE;
        }
        else if (clockConvertPolarityStr == "ACTIVE_LOW_OR_FALLING_EDGE") {
            clockConvertPolarity = XSERIES_AI_POLARITY_ACTIVE_LOW_OR_FALLING_EDGE;
        }
        else {
            ok = false;
            REPORT_ERROR(ErrorManagement::ParametersError, "Unsupported ClockConvertPolarity");
        }
    }
    StreamString scanIntervalCounterSourceStr;
    if (ok) {
        ok = data.Read("ScanIntervalCounterSource", scanIntervalCounterSourceStr);
        if (!ok) {
            REPORT_ERROR(ErrorManagement::ParametersError, "The ScanIntervalCounterSource shall be specified");
        }
    }
    if (ok) {
        if (scanIntervalCounterSourceStr == "COUNTER_TB3") {
            scanIntervalCounterSource = XSERIES_SCAN_INTERVAL_COUNTER_TB3;
        }
        else if (scanIntervalCounterSourceStr == "COUNTER_TB2") {
            scanIntervalCounterSource = XSERIES_SCAN_INTERVAL_COUNTER_TB2;
        }
        else if (scanIntervalCounterSourceStr == "COUNTER_TB1") {
            scanIntervalCounterSource = XSERIES_SCAN_INTERVAL_COUNTER_TB1;
        }
        else if (scanIntervalCounterSourceStr == "PFI0") {
            scanIntervalCounterSource = XSERIES_SCAN_INTERVAL_COUNTER_PFI0;
        }
        else if (scanIntervalCounterSourceStr == "PFI1") {
            scanIntervalCounterSource = XSERIES_SCAN_INTERVAL_COUNTER_PFI1;
        }
        else if (scanIntervalCounterSourceStr == "PFI2") {
            scanIntervalCounterSource = XSERIES_SCAN_INTERVAL_COUNTER_PFI2;
        }
        else if (scanIntervalCounterSourceStr == "PFI3") {
            scanIntervalCounterSource = XSERIES_SCAN_INTERVAL_COUNTER_PFI3;
        }
        else if (scanIntervalCounterSourceStr == "PFI4") {
            scanIntervalCounterSource = XSERIES_SCAN_INTERVAL_COUNTER_PFI4;
        }
        else if (scanIntervalCounterSourceStr == "PFI5") {
            scanIntervalCounterSource = XSERIES_SCAN_INTERVAL_COUNTER_PFI5;
        }
        else if (scanIntervalCounterSourceStr == "PFI6") {
            scanIntervalCounterSource = XSERIES_SCAN_INTERVAL_COUNTER_PFI6;
        }
        else if (scanIntervalCounterSourceStr == "PFI7") {
            scanIntervalCounterSource = XSERIES_SCAN_INTERVAL_COUNTER_PFI7;
        }
        else if (scanIntervalCounterSourceStr == "PFI8") {
            scanIntervalCounterSource = XSERIES_SCAN_INTERVAL_COUNTER_PFI8;
        }
        else if (scanIntervalCounterSourceStr == "PFI9") {
            scanIntervalCounterSource = XSERIES_SCAN_INTERVAL_COUNTER_PFI9;
        }
        else if (scanIntervalCounterSourceStr == "PFI10") {
            scanIntervalCounterSource = XSERIES_SCAN_INTERVAL_COUNTER_PFI10;
        }
        else if (scanIntervalCounterSourceStr == "PFI11") {
            scanIntervalCounterSource = XSERIES_SCAN_INTERVAL_COUNTER_PFI11;
        }
        else if (scanIntervalCounterSourceStr == "PFI12") {
            scanIntervalCounterSource = XSERIES_SCAN_INTERVAL_COUNTER_PFI12;
        }
        else if (scanIntervalCounterSourceStr == "PFI13") {
            scanIntervalCounterSource = XSERIES_SCAN_INTERVAL_COUNTER_PFI13;
        }
        else if (scanIntervalCounterSourceStr == "PFI14") {
            scanIntervalCounterSource = XSERIES_SCAN_INTERVAL_COUNTER_PFI14;
        }
        else if (scanIntervalCounterSourceStr == "PFI15") {
            scanIntervalCounterSource = XSERIES_SCAN_INTERVAL_COUNTER_PFI15;
        }
        else if (scanIntervalCounterSourceStr == "RTSI0") {
            scanIntervalCounterSource = XSERIES_SCAN_INTERVAL_COUNTER_RTSI0;
        }
        else if (scanIntervalCounterSourceStr == "RTSI1") {
            scanIntervalCounterSource = XSERIES_SCAN_INTERVAL_COUNTER_RTSI1;
        }
        else if (scanIntervalCounterSourceStr == "RTSI2") {
            scanIntervalCounterSource = XSERIES_SCAN_INTERVAL_COUNTER_RTSI2;
        }
        else if (scanIntervalCounterSourceStr == "RTSI3") {
            scanIntervalCounterSource = XSERIES_SCAN_INTERVAL_COUNTER_RTSI3;
        }
        else if (scanIntervalCounterSourceStr == "RTSI4") {
            scanIntervalCounterSource = XSERIES_SCAN_INTERVAL_COUNTER_RTSI4;
        }
        else if (scanIntervalCounterSourceStr == "RTSI5") {
            scanIntervalCounterSource = XSERIES_SCAN_INTERVAL_COUNTER_RTSI5;
        }
        else if (scanIntervalCounterSourceStr == "RTSI6") {
            scanIntervalCounterSource = XSERIES_SCAN_INTERVAL_COUNTER_RTSI6;
        }
        else if (scanIntervalCounterSourceStr == "RTSI7") {
            scanIntervalCounterSource = XSERIES_SCAN_INTERVAL_COUNTER_RTSI7;
        }
        else if (scanIntervalCounterSourceStr == "PXI_CLK10") {
            scanIntervalCounterSource = XSERIES_SCAN_INTERVAL_COUNTER_PXI_CLK10;
        }
        else if (scanIntervalCounterSourceStr == "STAR_TRIGGER") {
            scanIntervalCounterSource = XSERIES_SCAN_INTERVAL_COUNTER_STAR_TRIGGER;
        }
        else if (scanIntervalCounterSourceStr == "ANALOG_TRIGGER") {
            scanIntervalCounterSource = XSERIES_SCAN_INTERVAL_COUNTER_ANALOG_TRIGGER;
        }
        else if (scanIntervalCounterSourceStr == "DSTARA") {
            scanIntervalCounterSource = XSERIES_SCAN_INTERVAL_COUNTER_DSTARA;
        }
        else if (scanIntervalCounterSourceStr == "DSTARB") {
            scanIntervalCounterSource = XSERIES_SCAN_INTERVAL_COUNTER_DSTARB;
        }
        else {
            ok = false;
            REPORT_ERROR(ErrorManagement::ParametersError, "Unsupported ScanIntervalCounterSource");
        }
    }
    StreamString scanIntervalCounterPolarityStr;
    if (ok) {
        ok = data.Read("ScanIntervalCounterPolarity", scanIntervalCounterPolarityStr);
        if (!ok) {
            REPORT_ERROR(ErrorManagement::ParametersError, "The ScanIntervalCounterPolarity shall be specified");
        }
    }
    if (ok) {
        if (scanIntervalCounterPolarityStr == "RISING_EDGE") {
            scanIntervalCounterPolarity = XSERIES_SCAN_INTERVAL_COUNTER_POLARITY_RISING_EDGE;
        }
        else if (scanIntervalCounterPolarityStr == "FALLING_EDGE") {
            scanIntervalCounterPolarity = XSERIES_SCAN_INTERVAL_COUNTER_POLARITY_FALLING_EDGE;
        }
        else {
            ok = false;
            REPORT_ERROR(ErrorManagement::ParametersError, "Unsupported ScanIntervalCounterPolarity");
        }
    }

    if (ok) {
        if (!data.Read("CPUs", cpuMask)) {
            REPORT_ERROR_PARAMETERS(ErrorManagement::Information, "No CPUs defined for %s", GetName())
        }
    }
    //Get individual signal parameters
    uint32 i = 0u;
    if (ok) {
        ok = data.MoveRelative("Signals");
        if (!ok) {
            REPORT_ERROR(ErrorManagement::ParametersError, "Could not move to the Signals section");
        }
        if (ok) {
            //Do not allow to add signals in run-time
            ok = data.Write("Locked", 1);
        }
        while ((i < (NI6368ADC_MAX_CHANNELS + NI6368ADC_HEADER_SIZE)) && (ok)) {
            if (data.MoveRelative(data.GetChildName(i))) {
                uint32 channelId;
                if (data.Read("ChannelId", channelId)) {
                    ok = (channelId < NI6368ADC_MAX_CHANNELS);
                    if (!ok) {
                        REPORT_ERROR(ErrorManagement::ParametersError, "Invalid ChannelId specified.");
                    }
                    if (ok) {
                        adcEnabled[channelId] = true;
                        float32 range;
                        numberOfADCsEnabled++;
                        if (data.Read("InputRange", range)) {
                            if ((range > 9.99) && (range < 10.01)) {
                                inputRange[channelId] = XSERIES_INPUT_RANGE_10V;
                            }
                            else if ((range > 4.99) && (range < 5.01)) {
                                inputRange[channelId] = XSERIES_INPUT_RANGE_5V;
                            }
                            else if ((range > 1.99) && (range < 2.01)) {
                                inputRange[channelId] = XSERIES_INPUT_RANGE_2V;
                            }
                            else if ((range > 0.99) && (range < 1.01)) {
                                inputRange[channelId] = XSERIES_INPUT_RANGE_1V;
                            }
                            else if ((range > 0.499) && (range < 0.501)) {
                                inputRange[channelId] = XSERIES_INPUT_RANGE_500mV;
                            }
                            else if ((range > 0.199) && (range < 0.201)) {
                                inputRange[channelId] = XSERIES_INPUT_RANGE_200mV;
                            }
                            else if ((range > 0.099) && (range < 0.101)) {
                                inputRange[channelId] = XSERIES_INPUT_RANGE_100mV;
                            }
                            else {
                                ok = false;
                                REPORT_ERROR(ErrorManagement::ParametersError, "Unsupported InputRange.");
                            }
                        }
                        StreamString mode;
                        if (data.Read("InputType", mode)) {
                            if (mode == "Differential") {
                                inputType[channelId] = XSERIES_AI_CHANNEL_TYPE_DIFFERENTIAL;
                            }
                            else if (mode == "Loopback") {
                                inputType[channelId] = XSERIES_AI_CHANNEL_TYPE_LOOPBACK;
                            }
                            else if (mode == "Internal") {
                                inputType[channelId] = XSERIES_AI_CHANNEL_TYPE_INTERNAL;
                            }
                            else if (mode == "NRSE") {
                                inputType[channelId] = XSERIES_AI_CHANNEL_TYPE_NRSE;
                            }
                            else if (mode == "RSE") {
                                inputType[channelId] = XSERIES_AI_CHANNEL_TYPE_RSE;
                            }
                            else {
                                ok = false;
                                REPORT_ERROR(ErrorManagement::ParametersError, "Unsupported InputType.");
                            }
                        }
                    }
                }
                if (ok) {
                    ok = data.MoveToAncestor(1u);
                }
                i++;
            }
            else {
                break;
            }
        }
    }
    if (ok) {
        ok = data.MoveToAncestor(1u);
        if (!ok) {
            REPORT_ERROR(ErrorManagement::ParametersError, "Could not move to the parent section");
        }
    }
    return ok;
}

bool NI6368ADC::SetConfiguredDatabase(StructuredDataI& data) {
    uint32 i;
    bool ok = DataSourceI::SetConfiguredDatabase(data);
    if (ok) {
        ok = (GetNumberOfSignals() > (NI6368ADC_HEADER_SIZE));
    }
    if (!ok) {
        REPORT_ERROR_PARAMETERS(ErrorManagement::ParametersError, "At least (%d) signals shall be configured (header + 1 ADC)", NI6368ADC_HEADER_SIZE + 1u)
    }
    //The type of counter shall be unsigned int32 or uint32
    if (ok) {
        ok = (GetSignalType(0u) == SignedInteger32Bit);
        if (!ok) {
            ok = (GetSignalType(0u) == UnsignedInteger32Bit);
        }
        if (!ok) {
            REPORT_ERROR(ErrorManagement::ParametersError, "The first signal (counter) shall be of type SignedInteger32Bit or UnsignedInteger32Bit");
        }
    }
    //The type of time shall be unsigned int32 or uint32
    if (ok) {
        ok = (GetSignalType(1u) == SignedInteger32Bit);
        if (!ok) {
            ok = (GetSignalType(1u) == UnsignedInteger32Bit);
        }
        if (!ok) {
            REPORT_ERROR(ErrorManagement::ParametersError, "The second signal (time) shall be of type SignedInteger32Bit or UnsignedInteger32Bit");
        }
    }
    if (ok) {
        for (i = 0u; (i < numberOfADCsEnabled) && (ok); i++) {
            ok = (GetSignalType(NI6368ADC_HEADER_SIZE + i) == Float32Bit);
        }
        if (!ok) {
            REPORT_ERROR(ErrorManagement::ParametersError, "All the ADC signals shall be of type Float32Bit");
        }
    }

    uint32 nOfFunctions = GetNumberOfFunctions();
    uint32 functionIdx;
    //Check that the number of samples for the counter and the time is one and that for the other signals is always the same
    for (functionIdx = 0u; (functionIdx < nOfFunctions) && (ok); functionIdx++) {
        uint32 nOfSignals = 0u;
        ok = GetFunctionNumberOfSignals(InputSignals, functionIdx, nOfSignals);

        for (i = 0u; (i < nOfSignals) && (ok); i++) {
            bool isCounter = false;
            bool isTime = false;
            uint32 signalIdx = 0u;
            uint32 nSamples = 0u;
            ok = GetFunctionSignalSamples(InputSignals, functionIdx, i, nSamples);

            //Is the counter or the time signal?
            StreamString signalAlias;
            if (ok) {
                ok = GetFunctionSignalAlias(InputSignals, functionIdx, i, signalAlias);
            }
            if (ok) {
                ok = GetSignalIndex(signalIdx, signalAlias.Buffer());
            }
            if (ok) {
                isCounter = (signalIdx == 0u);
                isTime = (signalIdx == 1u);
            }
            if (ok) {
                if (isCounter) {
                    if (nSamples > 1u) {
                        ok = false;
                        REPORT_ERROR(ErrorManagement::ParametersError, "The first signal (counter) shall have one and only one sample");
                    }
                }
                else if (isTime) {
                    if (nSamples > 1u) {
                        ok = false;
                        REPORT_ERROR(ErrorManagement::ParametersError, "The second signal (time) shall have one and only one sample");
                    }
                }
                else {
                    if (numberOfSamples == 0u) {
                        numberOfSamples = nSamples;
                    }
                    else {
                        if (numberOfSamples != nSamples) {
                            ok = false;
                            REPORT_ERROR(ErrorManagement::ParametersError, "All the ADC signals shall have the same number of samples");
                        }
                    }

                }
            }
        }
    }
    if (ok) {
        if (synchronising) {
            //numberOfADCsEnabled > 0 as otherwise it would have stopped before
            if (numberOfADCsEnabled > 0u) {
                float32 totalNumberOfSamplesPerSecond = (static_cast<float32>(numberOfSamples) * cycleFrequency);
                ok = (NI6368ADC_SAMPLING_FREQUENCY == static_cast<uint32>(totalNumberOfSamplesPerSecond));
                if (!ok) {
                    REPORT_ERROR_PARAMETERS(ErrorManagement::ParametersError,
                                            "The numberOfSamples * cycleFrequency (%u) shall be equal to the ADCs acquisition frequency (%u)",
                                            totalNumberOfSamplesPerSecond, NI6368ADC_SAMPLING_FREQUENCY)
                }
            }
        }
    }
    StreamString fullDeviceName;
    //Configure the board
    if (ok) {
        ok = fullDeviceName.Printf("%s.%d.ai", deviceName.Buffer(), boardId);
    }
    if (ok) {
        ok = fullDeviceName.Seek(0LLU);
    }
    if (ok) {
        boardFileDescriptor = open(fullDeviceName.Buffer(), O_RDWR);
        ok = (boardFileDescriptor > -1);
        if (!ok) {
            REPORT_ERROR_PARAMETERS(ErrorManagement::ParametersError, "Could not open device %s", fullDeviceName)
        }
    }
    if (ok) {
        bool stopped = (xseries_stop_ai(boardFileDescriptor) == 0);
        if (!stopped) {
            REPORT_ERROR_PARAMETERS(ErrorManagement::Warning, "Could not stop the device %s while starting", fullDeviceName)
        }
        bool reset = (xseries_reset_ai(boardFileDescriptor) == 0);
        if (!reset) {
            REPORT_ERROR_PARAMETERS(ErrorManagement::Warning, "Could not reset the device %s while starting", fullDeviceName)
        }
    }
    xseries_ai_conf_t adcConfiguration = xseries_continuous_ai();
    for (i = 0u; (i < NI6368ADC_MAX_CHANNELS) && (ok); i++) {
        if (adcEnabled[i]) {
            ok = (xseries_add_ai_channel(&adcConfiguration, static_cast<uint8_t>(i), inputRange[i], inputType[i], 0u) == 0);
            uint32 ii = i;
            if (!ok) {
                REPORT_ERROR_PARAMETERS(ErrorManagement::ParametersError, "Could not set InputRange for channel %d of device %s", ii, fullDeviceName)
            }
        }
    }
    if (ok) {
        ok = (xseries_set_ai_sample_clock(&adcConfiguration, clockSampleSource, clockSamplePolarity, 1u) == 0);
        if (!ok) {
            REPORT_ERROR_PARAMETERS(ErrorManagement::ParametersError, "Could not set the sample clock for device %s", fullDeviceName)
        }
    }
    if (ok) {
        ok = (xseries_set_ai_convert_clock(&adcConfiguration, clockConvertSource, clockConvertPolarity) == 0);
        if (!ok) {
            REPORT_ERROR_PARAMETERS(ErrorManagement::ParametersError, "Could not set the convert clock for device %s", fullDeviceName)
        }
    }
    if (ok) {
        ok = (xseries_set_ai_scan_interval_counter(&adcConfiguration, scanIntervalCounterSource, scanIntervalCounterPolarity, scanIntervalCounterPeriod, scanIntervalCounterDelay) == 0);
        if (!ok) {
            REPORT_ERROR_PARAMETERS(ErrorManagement::ParametersError, "Could not set the convert clock for device %s", fullDeviceName)
        }
    }
    if (ok) {
        ok = (xseries_load_ai_conf(boardFileDescriptor, adcConfiguration) == 0);
        if (!ok) {
            REPORT_ERROR_PARAMETERS(ErrorManagement::ParametersError, "Could not load configuration for device %s", fullDeviceName)
        }
    }
    if (ok) {
        //Allocate memory
        for (i = 0u; (i < NI6368ADC_MAX_CHANNELS) && (ok); i++) {
            channelsMemory[i] = new float32[numberOfSamples];
        }
        channelMemory = new float32[numberOfSamples];
    }

    if (ok) {
        //Required to wait for devices to be available in /dev!
        Sleep::Sec(1.0);
        for (i = 0u; (i < NI6368ADC_MAX_CHANNELS) && (ok); i++) {
            if (adcEnabled[i]) {
                StreamString channelDeviceName;
                //Otherwise there is the perception that the Printf might modify i inside the for loop
                uint32 ii = i;
                ok = channelDeviceName.Printf("%s.%d", fullDeviceName.Buffer(), ii);
                if (ok) {
                    ok = channelDeviceName.Seek(0ULL);
                }
                if (ok) {
                    channelsFileDescriptors[i] = open(channelDeviceName.Buffer(), O_RDWR);
                    ok = (channelsFileDescriptors[i] > -1);
                    if (!ok) {
                        REPORT_ERROR_PARAMETERS(ErrorManagement::ParametersError, "Could not open device %s", channelDeviceName)
                    }
                }
            }
        }
    }
    if (ok) {
        ok = (xseries_start_ai(boardFileDescriptor) == 0);
        if (!ok) {
            REPORT_ERROR_PARAMETERS(ErrorManagement::ParametersError, "Could not start the device %s", fullDeviceName)
        }
    }
    return ok;
}

ErrorManagement::ErrorType NI6368ADC::Execute(const ExecutionInfo& info) {
    ErrorManagement::ErrorType err;
    if (info.GetStage() == ExecutionInfo::TerminationStage) {
        keepRunning = false;
    }
    else {
        uint32 i;
        for (i = 0u; (i < NI6368ADC_MAX_CHANNELS) && (keepRunning); i++) {
            if (adcEnabled[i]) {
                size_t readSamples = 0u;
                while ((readSamples < numberOfSamples) && (keepRunning)) {
                    size_t leftSamples = static_cast<size_t>(numberOfSamples) - readSamples;
                    //ssize_t currentSamples = pxi6368_read_ai(channelsFileDescriptors[i], &(channelsMemory[i][readSamples]), leftSamples);
                    //Unfortunately a buffered memory has to be used to read from the ADC. Using directly channelsMemory[i][readSamples] was
                    //corrupting the memory if this was being copied by the broker while pxi6368_read_ai was being called.
                    if (channelMemory != NULL_PTR(float32 *)) {
                        ssize_t currentSamples = xseries_read_ai(channelsFileDescriptors[i], &(channelMemory[readSamples]), leftSamples);
                        if (currentSamples > 0) {
                            readSamples += static_cast<size_t>(currentSamples);
                            //Needs to sleep while waiting for data, otherwise it will get stuck on pxi6368_read_ai.
                            //Do not try to further optimise this (e.g. by only sleeping once or so, as it will stop working!).
                            if (i == 0u) {
                                Sleep::Sec(20e-6);
                            }
                        }
                        else {
                            Sleep::Sec(100e-6);
                            REPORT_ERROR(ErrorManagement::ParametersError, "Failed reading from ADC");
                        }
                    }
                }
            }
            if (keepRunning) {
                keepRunning = MemoryOperationsHelper::Copy(channelsMemory[i], channelMemory, numberOfSamples * static_cast<uint32>(sizeof(float32)));
            }
        }
        if (synchronising) {
            err = !synchSem.Post();
        }
        counter++;
        timeValue = counter * numberOfSamples / NI6368ADC_SAMPLING_FREQUENCY;
    }

    return err;
}

bool NI6368ADC::ReadAIConfiguration(xseries_ai_conf_t * const conf) const {
    bool ok = false;
    if (boardFileDescriptor > 0) {
        ok = (xseries_read_ai_conf(boardFileDescriptor, conf) == 0);
    }
    return ok;
}

CLASS_REGISTER(NI6368ADC, "1.0")
}

