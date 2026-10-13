// SPDX-License-Identifier: MIT
#ifndef IntelGtBlitUserClient_hpp
#define IntelGtBlitUserClient_hpp
#include <IOKit/IOUserClient.h>
class IntelGtBringup;
class IntelGtBlitUserClient : public IOUserClient {
    OSDeclareDefaultStructors(IntelGtBlitUserClient)
public:
    bool initWithTask(task_t task, void *securityID, UInt32 type,
                      OSDictionary *properties) override;
    bool start(IOService *provider) override;
    void free(void) override;
    IOReturn clientClose(void) override;
    IOReturn externalMethod(UInt32 selector, IOExternalMethodArguments *arguments,
                            IOExternalMethodDispatch *dispatch = nullptr,
                            OSObject *target = nullptr, void *reference = nullptr) override;
private:
    IntelGtBringup *fOwner;
};
#endif
