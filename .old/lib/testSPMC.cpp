#include <chrono>
#include <cstdio>
#include <thread>
#include "spmc.hpp"
#include "shared.hpp"

using Queue = SPMCQueue<double, 4096>;
void run(const char* read_name, Queue& write_q, Queue& read_q){
    auto reader = read_q.getReader();
    double value = 0.0;
    char msg[64];
    while(1){
        write_q.write([&](double& d){ d = value; });
        value += 0.01;
        if(double* d = reader.readLast()){
            std::snprintf(msg, sizeof(msg), "Read %.2f from buffer %s", *d, read_name);
            tLog(msg);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
};
int main(){
    SPMCQueue<double, 4096> A, B;
    std::thread x(run, "B", std::ref(A), std::ref(B));
    std::thread y(run, "A", std::ref(B), std::ref(A));
    x.join();
    y.join();
};
