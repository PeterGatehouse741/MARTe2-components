/**
 * @file OPCUAServer.cpp
 * @brief Source file for class OPCUAServer
 * @date 24/01/2019
 * @author Luca Porzio
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
 * the class OPCUAServer (public, protected, and private). Be aware that some 
 * methods, such as those inline could be defined on the header file, instead.
 */

#define DLL_API

/*---------------------------------------------------------------------------*/
/*                         Standard header includes                          */
/*---------------------------------------------------------------------------*/

/*---------------------------------------------------------------------------*/
/*                         Project header includes                           */
/*---------------------------------------------------------------------------*/

#include "OPCUAServer.h"

/*---------------------------------------------------------------------------*/
/*                           Static definitions                              */
/*---------------------------------------------------------------------------*/

/*---------------------------------------------------------------------------*/
/*                           Method definitions                              */
/*---------------------------------------------------------------------------*/
/*-e909 and -e9133 redefines bool*/
/*lint -save -e909 -e9133*/
namespace MARTe {

OPCUAServer::OPCUAServer() :
        Object(),
        EmbeddedServiceMethodBinderI(),
        service(*this) {
    opcuaConfig = NULL_PTR(UA_ServerConfig *);
    opcuaServer = NULL_PTR(UA_Server *);
    opcuaRunning = false;
    port = 4840u;
    cpuMask = 0xffu;
    stackSize = THREADS_DEFAULT_STACKSIZE;
    nodeNumber = 3000u;
}

OPCUAServer::~OPCUAServer() {
    SetRunning(false);
    (void) service.Stop();
}

bool OPCUAServer::Initialise(StructuredDataI &data) {
    bool ok = Object::Initialise(data);
    if (ok) {
        ok = data.Copy(cdb);
    }
    if (ok) {
        ok = data.Read("Port", port);
        if (!ok) {
            REPORT_ERROR(ErrorManagement::Information, "No Port number defined. It will be 4840.");
            ok = true;
        }
    }
    if (ok) {
        ok = data.Read("CPUMask", cpuMask);
        if (!ok) {
            REPORT_ERROR(ErrorManagement::Information, "No CPUMask defined. It will be default one.");
            ok = true;
        }
    }
    if (ok) {
        ok = data.Read("StackSize", stackSize);
        if (!ok) {
            REPORT_ERROR(ErrorManagement::Information, "No StackSize defined. It will be default.");
            ok = true;
        }
    }
    if (ok) {
        opcuaConfig = UA_ServerConfig_new_minimal(port, NULL_PTR(const UA_ByteString *));
        opcuaConfig->maxSessions = 1000u;
        opcuaConfig->maxSecureChannels = 1000u;
        opcuaServer = UA_Server_new(opcuaConfig);
    }
    if (ok) {
        ok = cdb.MoveRelative("AddressSpace");
        if (!ok) {
            REPORT_ERROR(ErrorManagement::ParametersError, "No Address Space defined!");
        }
    }
    if (ok) {
        service.SetCPUMask(cpuMask);
        service.SetStackSize(stackSize);
        ok = (service.Start() == ErrorManagement::NoError);
    }
    return ok;
}

ErrorManagement::ErrorType OPCUAServer::Execute(ExecutionInfo & info) {
    ErrorManagement::ErrorType err = ErrorManagement::NoError;
    if (info.GetStage() == ExecutionInfo::StartupStage) {
        REPORT_ERROR(ErrorManagement::Information, "OPCUAServer Startup Stage");
        //Here read the structure and Initialise the Address Space
        bool ok = false;
        uint32 nOfChildren = cdb.GetNumberOfChildren();
        StreamString typeStr;
        uint32 i;
        for (i = 0u; i < nOfChildren; i++) {
            ok = cdb.MoveToChild(i);
            if (ok) {
                ok = cdb.Read("Type", typeStr);
                if (!ok) {
                    REPORT_ERROR(ErrorManagement::ParametersError, "A Type shall be defined for every node");
                }
            }
            if (ok) {
                ok = cdb.MoveToAncestor(1u);
            }
            if (ok) {
                const ClassRegistryItem *cri = ClassRegistryDatabase::Instance()->Find(typeStr.Buffer());
                if (cri != NULL_PTR(const ClassRegistryItem *)) {
                    ReferenceT<OPCUAObject> mainObject("OPCUAObject", GlobalObjectsDatabase::Instance()->GetStandardHeap());
                    mainObject->SetName(cdb.GetChildName(i));
                    mainObject->SetFirst(true);
                    const Introspection *intro = cri->GetIntrospection();
                    ok = (intro != NULL_PTR(const Introspection *));
                    if (ok) {
                        ok = GetStructure(mainObject, intro);
                        if (ok) {
                            ok = InitAddressSpace(mainObject);
                        }
                    }
                }
                else {
                    ReferenceT<OPCUANode> mainNode("OPCUANode", GlobalObjectsDatabase::Instance()->GetStandardHeap());
                    mainNode->SetName(cdb.GetChildName(i));
                    TypeDescriptor td = TypeDescriptor::GetTypeDescriptorFromTypeName(typeStr.Buffer());
                    mainNode->SetNodeType(td);
                    if (cdb.MoveToChild(0u)) {
                        uint32 nElem = 1u;
                        if (cdb.Read("NumberOfElements", nElem)) {
                            mainNode->SetNumberOfDimensions(1u);
                            mainNode->SetNumberOfElements(0u, nElem);
                        }
                        REPORT_ERROR(ErrorManagement::Information, "Number Of Elements = %d", nElem);
                    }
                    ok = InitAddressSpace(mainNode);
                    if (ok) {
                        ok = cdb.MoveToAncestor(1u);
                    }

                }
            }
            if (ok) {
                SetRunning(true);
            }
            if (!ok) {
                REPORT_ERROR(ErrorManagement::ParametersError, "Cannot initialise Address Space");
            }
        }
        //This is a blocking call
        if (ok) {
            UA_StatusCode code = UA_Server_run(opcuaServer, &opcuaRunning);
            code = 0x00U;
        }
        UA_Server_delete(opcuaServer);
        UA_ServerConfig_delete(opcuaConfig);
    }
    else if (info.GetStage() != ExecutionInfo::BadTerminationStage) {
        SetRunning(false);
        UA_Server_delete(opcuaServer);
        UA_ServerConfig_delete(opcuaConfig);
    }
    else if (info.GetStage() != ExecutionInfo::AsyncTerminationStage) {
        SetRunning(false);
        UA_Server_delete(opcuaServer);
        UA_ServerConfig_delete(opcuaConfig);
    }
    else {
        SetRunning(false);
        UA_Server_delete(opcuaServer);
        UA_ServerConfig_delete(opcuaConfig);
    }
    return err;
}

bool OPCUAServer::InitAddressSpace(ReferenceT<OPCUAReferenceContainer> ref) {
    bool ok = true;
    UA_StatusCode code = 1u;
    //const char* parentId = NULL_PTR(const char*);
    uint32 parentId = 0u;
    if (ref->IsObject()) {
        OPCUAObjectSettings settings = new ObjectProperties;
        ok = ref->GetOPCObject(settings, nodeNumber);
        do {
            ok = ref->IsFirstObject();
            if (ok) {
                code = UA_Server_addObjectNode(opcuaServer, settings->nodeId, settings->parentNodeId, settings->parentReferenceNodeId, settings->nodeName,
                                               UA_NODEID_NUMERIC(0u, 61u), settings->attr, NULL_PTR(void*), NULL_PTR(UA_NodeId *));
            }
            else {
                code = UA_Server_addObjectNode(opcuaServer, settings->nodeId, settings->parentNodeId, settings->parentReferenceNodeId, settings->nodeName,
                                               UA_NODEID_NUMERIC(0u, 58u), settings->attr, NULL_PTR(void*), NULL_PTR(UA_NodeId *));
            }
            if (code == 0x805E0000U) {
                nodeNumber++;
                ref->SetNodeId(nodeNumber);
                settings->nodeId = UA_NODEID_NUMERIC(1u, nodeNumber);
            }
            else {
                parentId = ref->GetNodeId();
            }
        }
        while (code != 0x00U);
        delete settings;
    }
    else if (ref->IsNode()) {
        TypeDescriptor typeName = ref->GetNodeType();
        OPCUANodeSettings settings = new NodeProperties;
        ok = ref->GetOPCVariable(settings, typeName, nodeNumber);
        do {
            if (ok) {
                code = UA_Server_addVariableNode(opcuaServer, settings->nodeId, settings->parentNodeId, settings->parentReferenceNodeId, settings->nodeName,
                                                 UA_NODEID_NUMERIC(0u, 63u), settings->attr, NULL_PTR(void *), NULL_PTR(UA_NodeId *));
            }
            if (code == 0x805E0000U) {
                nodeNumber++;
                ref->SetNodeId(nodeNumber);
                settings->nodeId = UA_NODEID_NUMERIC(1u, nodeNumber);
            }
        }
        while (code != 0x00U);
        delete settings;
    }
    else {
        REPORT_ERROR(ErrorManagement::ParametersError, "Problem occurred during Address Space Initialisation. Is the node type correct?");
    }
    uint32 i;
    uint32 size = ref->Size();
    nodeNumber++;
    for (i = 0u; (i < size) && (ok); i++) {
        ReferenceT<OPCUAReferenceContainer> rc = ref->Get(i);
        if (rc.IsValid()) {
            rc->SetParent(parentId);
            ok = InitAddressSpace(rc);
        }
    }

    return (code == 0x00U);
}

bool OPCUAServer::GetStructure(ReferenceT<OPCUAReferenceContainer> refContainer,
                               const Introspection *intro) {
    bool ok = true;
    uint32 numberOfMembers = intro->GetNumberOfMembers();
    uint32 j;
    for (j = 0u; j < numberOfMembers; j++) {
        const IntrospectionEntry entry = intro->operator[](j);
        const char8 * const memberName = entry.GetMemberName();
        const char8 * const memberTypeName = entry.GetMemberTypeName();
        TypeDescriptor td = entry.GetMemberTypeDescriptor();
        bool isStructured = entry.GetMemberTypeDescriptor().isStructuredData;
        if (isStructured) {
            ReferenceT<OPCUAObject> node("OPCUAObject", GlobalObjectsDatabase::Instance()->GetStandardHeap());
            //node->SetNodeType(td);
            node->SetName(memberName);
            ok = refContainer->Insert(node);
            const ClassRegistryItem *cri = ClassRegistryDatabase::Instance()->Find(memberTypeName);
            if (ok) {
                ok = (cri != NULL_PTR(const ClassRegistryItem *));
            }
            if (ok) {
                ok = GetStructure(node, cri->GetIntrospection());
            }
        }
        else {
            ReferenceT<OPCUANode> finalNode("OPCUANode", GlobalObjectsDatabase::Instance()->GetStandardHeap());
            uint8 nDimensions = entry.GetNumberOfDimensions();
            if (nDimensions > 1u) {
                REPORT_ERROR(ErrorManagement::ParametersError,
                             "the Introspection entry [%s] has Number of Dimensions = %d. Multidimensional array not supported yet. ", memberName, nDimensions);
                ok = false;
            }
            if (ok) {
                finalNode->SetNumberOfDimensions(nDimensions);
                for (uint32 i = 0u; i < nDimensions; i++) {
                    finalNode->SetNumberOfElements(i, entry.GetNumberOfElements(i));
                }
                finalNode->SetNodeType(td);
                finalNode->SetName(memberName);
                ok = refContainer->Insert(finalNode);
            }
        }
    }
    return ok;
}

void OPCUAServer::SetRunning(bool const running) {
    opcuaRunning = running;
}

const bool OPCUAServer::GetRunning() const {
    return opcuaRunning;
}

const uint32 OPCUAServer::GetCPUMask() const {
    return cpuMask;
}

const uint32 OPCUAServer::GetStackSize() const {
    return stackSize;
}

const uint16 OPCUAServer::GetPort() const {
    return port;
}

CLASS_REGISTER(OPCUAServer, "");

}
/*lint -restore*/
