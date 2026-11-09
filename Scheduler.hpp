//
//  Scheduler.hpp
//  CloudSim
//
//  Created by ELMOOTAZBELLAH ELNOZAHY on 10/20/24.
//

#ifndef Scheduler_hpp
#define Scheduler_hpp

#include <vector>

#include "Interfaces.h"

class Scheduler {
public:
    Scheduler()                 {}
    void Init();
    void MigrationComplete(Time_t time, VMId_t vm_id);
    void NewTask(Time_t now, TaskId_t task_id);
    void PeriodicCheck(Time_t now);
    void Shutdown(Time_t now);
    void TaskComplete(Time_t now, TaskId_t task_id);
    VMId_t FindOrCreateVM(CPUType_t cpu_type, VMType_t vm_type, MachineId_t machine_id);
    void SLAWarning_Handler(Time_t time, TaskId_t task_id);
private:
    void pegasus_periodic();
    vector<VMId_t> vms;
    vector<MachineId_t> machines;
};



#endif /* Scheduler_hpp */
