// csrc/test_issue.cpp
#include "Vtb_issue.h"
#include "verilated.h"
#include <cassert>
#include <iostream>
#include <vector>
#include <iomanip>

// =================================================================
// 配置参数
// =================================================================
const int RS_DEPTH = 16;
const int INSTR_PER_FETCH = 4;

vluint64_t main_time = 0;

void tick(Vtb_issue *top) {
    top->clk = 0;
    top->eval();
    main_time++;
    top->clk = 1;
    top->eval();
    main_time++;
}

// 辅助结构：用于定义一条要 Dispatch 的指令
struct DispatchInstr {
    bool valid;
    uint32_t op;
    uint32_t dst_tag;
    uint32_t v1;
    uint32_t q1;
    bool r1;
    uint32_t v2;
    uint32_t q2;
    bool r2;
};

// 辅助函数：设置 Dispatch 输入端口
void set_dispatch(Vtb_issue *top, const std::vector<DispatchInstr>& instrs) {
    // 先清零
    top->dispatch_valid = 0;
    for (int i = 0; i < INSTR_PER_FETCH; ++i) {
        top->dispatch_op[i] = 0;
        top->dispatch_dst[i] = 0;
        top->dispatch_v1[i] = 0;
        top->dispatch_q1[i] = 0;
        top->dispatch_r1[i] = 0;
        top->dispatch_v2[i] = 0;
        top->dispatch_q2[i] = 0;
        top->dispatch_r2[i] = 0;
    }

    // 设置有效值
    uint8_t valid_mask = 0;
    for (size_t i = 0; i < instrs.size() && i < 4; ++i) {
        if (instrs[i].valid) {
            valid_mask |= (1 << i);
            top->dispatch_op[i]  = instrs[i].op;
            top->dispatch_dst[i] = instrs[i].dst_tag;
            top->dispatch_v1[i]  = instrs[i].v1;
            top->dispatch_q1[i]  = instrs[i].q1;
            top->dispatch_r1[i]  = instrs[i].r1;
            top->dispatch_v2[i]  = instrs[i].v2;
            top->dispatch_q2[i]  = instrs[i].q2;
            top->dispatch_r2[i]  = instrs[i].r2;
        }
    }
    top->dispatch_valid = valid_mask;
}

// 辅助函数：设置 CDB 广播
void set_cdb(Vtb_issue *top, const std::vector<std::pair<uint32_t, uint32_t>>& updates) {
    top->cdb_valid = 0;
    for(int i=0; i<4; ++i) {
        top->cdb_tag[i] = 0;
        top->cdb_val[i] = 0;
    }

    uint8_t valid_mask = 0;
    for (size_t i = 0; i < updates.size() && i < 4; ++i) {
        valid_mask |= (1 << i);
        top->cdb_tag[i] = updates[i].first;  // Tag
        top->cdb_val[i] = updates[i].second; // Value
    }
    top->cdb_valid = valid_mask;
}

int main(int argc, char **argv) {
    Verilated::commandArgs(argc, argv);
    Vtb_issue *top = new Vtb_issue;

    std::cout << "--- [START] Issue Stage Verification ---" << std::endl;

    // 1. 复位
    top->rst_n = 0;
    top->clk = 0;
    set_dispatch(top, {});
    set_cdb(top, {});
    tick(top);
    top->rst_n = 1;
    tick(top); // 等待复位释放

    std::cout << "[" << main_time << "] Reset complete. issue_ready = " << (int)top->issue_ready << std::endl;
    assert(top->issue_ready == 1 && "Should be ready after reset");

    // =================================================================
    // Test 1: 发射 2 条已就绪的指令 (Direct Issue)
    // =================================================================
    std::cout << "\n--- Test 1: Dispatch Ready Instructions ---" << std::endl;
    
    // 使用合法的十六进制数
    // 0xADD00001 (ADD) -> OK
    // 0x50B00002 (SUB) -> OK (50B looks like SOB/SUB)
    uint32_t OP_ADD = 0xADD00001;
    uint32_t OP_SUB = 0x50B00002;

    std::vector<DispatchInstr> group1 = {
        {true, OP_ADD, 3, 100, 0, 1, 200, 0, 1}, // Op, DstTag, V1, Q1, R1, V2, Q2, R2
        {true, OP_SUB, 6, 300, 0, 1, 400, 0, 1}
    };
    set_dispatch(top, group1);
    
    // Tick 1: Dispatch Write -> RS Busy 变高
    tick(top); 
    
    // 撤销 Dispatch 请求
    set_dispatch(top, {});

    // 检查是否发射
    top->eval(); 
    
    bool executed_0 = false;
    bool executed_1 = false;

    // 我们给一点时间让它们发射
    for(int i=0; i<3; ++i) {
        if (top->alu0_en) {
            std::cout << "  [Cycle " << i << "] ALU0 Fire! Op=0x" << std::hex << top->alu0_op << std::dec << std::endl;
            if (top->alu0_op == OP_ADD) executed_0 = true;
            if (top->alu0_op == OP_SUB) executed_1 = true;
        }
        if (top->alu1_en) {
            std::cout << "  [Cycle " << i << "] ALU1 Fire! Op=0x" << std::hex << top->alu1_op << std::endl;
            if (top->alu1_op == OP_ADD) executed_0 = true;
            if (top->alu1_op == OP_SUB) executed_1 = true;
        }
        tick(top);
    }

    assert(executed_0 && "Instr 0 failed to issue");
    assert(executed_1 && "Instr 1 failed to issue");
    std::cout << "--- Test 1 PASSED ---" << std::endl;


    // =================================================================
    // Test 2: 依赖等待测试 (Wait for CDB)
    // =================================================================
    std::cout << "\n--- Test 2: Dependency & CDB Wakeup ---" << std::endl;

    // 使用合法的十六进制数
    uint32_t OP_WAIT_A = 0x000000AA; // WAIT A
    uint32_t OP_WAIT_B = 0x000000BB; // WAIT B
    uint32_t DATA_10   = 0xDA7A0010; // DATA 10
    uint32_t DATA_11   = 0xDA7A0011; // DATA 11

    // 发射 2 条未就绪指令
    // Instr A: 依赖 Tag 10 (Q1=10, R1=0)
    // Instr B: 依赖 Tag 11 (Q2=11, R2=0)
    std::vector<DispatchInstr> group2 = {
        {true, OP_WAIT_A, 20, 0, 10, 0, 500, 0, 1}, // Src1 not ready (Wait Tag 10)
        {true, OP_WAIT_B, 21, 600, 0, 1, 0, 11, 0}  // Src2 not ready (Wait Tag 11)
    };
    set_dispatch(top, group2);
    tick(top);
    set_dispatch(top, {}); // Stop dispatch

    // 运行几个周期，确保它们没有提前发射
    for(int i=0; i<3; ++i) {
        top->eval();
        if (top->alu0_en || top->alu1_en) {
            std::cout << "[ERROR] Instruction issued before operands were ready!" << std::endl;
            assert(false);
        }
        tick(top);
    }
    std::cout << "  [Verified] Instructions held in RS waiting for operands." << std::endl;

    // 模拟 CDB 广播 Tag 10 和 11
    std::cout << "  [Action] Broadcasting CDB Tag 10 and 11..." << std::endl;
    std::vector<std::pair<uint32_t, uint32_t>> cdb_data = {
        {10, DATA_10}, 
        {11, DATA_11}
    };
    set_cdb(top, cdb_data);
    
    // CDB 写入是同步的，需要一个 Tick
    tick(top); 
    set_cdb(top, {}); // Clear CDB

    // 现在指令应该已经捕获数据并 Ready 了，Select 逻辑应该选中它们
    bool fired_a = false;
    bool fired_b = false;

    for(int i=0; i<5; ++i) {
        top->eval();
        if (top->alu0_en) {
            if (top->alu0_op == OP_WAIT_A) {
                std::cout << "  ALU0 Issued Instr A. V1=" << std::hex << top->alu0_v1 << " (Expect " << DATA_10 << ")" << std::endl;
                assert(top->alu0_v1 == DATA_10); // 验证 Forwarding 结果
                fired_a = true;
            }
            if (top->alu0_op == OP_WAIT_B) {
                std::cout << "  ALU0 Issued Instr B. V2=" << std::hex << top->alu0_v2 << " (Expect " << DATA_11 << ")" << std::endl;
                assert(top->alu0_v2 == DATA_11);
                fired_b = true;
            }
        }
        if (top->alu1_en) {
             if (top->alu1_op == OP_WAIT_A) {
                std::cout << "  ALU1 Issued Instr A. V1=" << std::hex << top->alu1_v1 << std::endl;
                assert(top->alu1_v1 == DATA_10);
                fired_a = true;
            }
            if (top->alu1_op == OP_WAIT_B) {
                std::cout << "  ALU1 Issued Instr B. V2=" << std::hex << top->alu1_v2 << std::endl;
                assert(top->alu1_v2 == DATA_11);
                fired_b = true;
            }
        }
        tick(top);
    }

    assert(fired_a && fired_b && "Dependent instructions failed to issue after CDB wakeup");
    std::cout << "--- Test 2 PASSED ---" << std::endl;

    // =================================================================
    // Test 3: RS 满状态与阻塞 (Full Stall)
    // =================================================================
    std::cout << "\n--- Test 3: RS Full Stall Check ---" << std::endl;
    // 目前 RS 应该是空的。RS_DEPTH = 16.
    // 我们连续填入 16 条 "卡住" 的指令 (依赖一个永不到来的 Tag 99)
    
    uint32_t OP_STALL = 0x57A11000; // STALL
    DispatchInstr stall_instr = {true, OP_STALL, 99, 0, 99, 0, 0, 99, 0};
    std::vector<DispatchInstr> batch(4, stall_instr); // 每次发 4 条

    // 发射 4 次，共 16 条
    for(int i=0; i<4; ++i) {
        std::cout << "  Filling Batch " << i+1 << " (Ready=" << (int)top->issue_ready << ")" << std::endl;
        assert(top->issue_ready == 1); // 此时应该还没满
        set_dispatch(top, batch);
        tick(top);
    }
    set_dispatch(top, {});
    
    // 此时 16 条槽位应该全满
    top->eval();
    std::cout << "  [Check] RS Full. issue_ready = " << (int)top->issue_ready << std::endl;
    assert(top->issue_ready == 0); // 必须为 0

    // 尝试在满的时候强行发射 (应该被忽略)
    std::cout << "  [Action] Attempting dispatch when FULL..." << std::endl;
    
    uint32_t OP_NEW = 0x000000FF; // NEW
    DispatchInstr new_instr = {true, OP_NEW, 50, 0, 0, 1, 0, 0, 1}; // 这是一条 Ready 指令
    set_dispatch(top, {new_instr, new_instr, new_instr, new_instr});
    tick(top);
    set_dispatch(top, {}); // Stop

    // 检查是否偷跑进去了
    // 如果进了，由于它是 Ready 的，它应该会马上发射。如果不发射，说明没进去。
    for(int i=0; i<3; ++i) {
        if (top->alu0_en && top->alu0_op == OP_NEW) assert(false && "Dispatch accepted while RS FULL!");
        if (top->alu1_en && top->alu1_op == OP_NEW) assert(false && "Dispatch accepted while RS FULL!");
        tick(top);
    }
    std::cout << "  [Verified] No instructions accepted while FULL." << std::endl;

    // 释放一些空间 (通过 CDB 唤醒刚才填进去的指令)
    std::cout << "  [Action] Releasing instructions via CDB Tag 99..." << std::endl;
    set_cdb(top, {{99, 0xDEADBEEF}});
    tick(top);
    set_cdb(top, {});

    // 等待它们发射并离开 RS
    int fired_count = 0;
    for(int i=0; i<20; ++i) {
        if (top->alu0_en) fired_count++;
        if (top->alu1_en) fired_count++;
        tick(top);
    }
    std::cout << "  [Info] Fired " << fired_count << " instructions after release." << std::endl;
    
    // 此时 RS 应该有空位了，issue_ready 应该恢复
    top->eval();
    std::cout << "  [Check] issue_ready = " << (int)top->issue_ready << std::endl;
    assert(top->issue_ready == 1);

    std::cout << "--- Test 3 PASSED ---" << std::endl;

    std::cout << "\n--- [SUCCESS] All Issue Stage Tests Passed! ---" << std::endl;

    delete top;
    return 0;
}