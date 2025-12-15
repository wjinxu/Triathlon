module issue #(
    parameter config_pkg::cfg_t Cfg = config_pkg::EmptyCfg,
    parameter RS_DEPTH = Cfg.RS_DEPTH,
    parameter DATA_W   = Cfg.ILEN,
    parameter TAG_W    = 6
)(
    input wire clk,
    input wire rst_n,

    input wire [3:0]        dispatch_valid,
    input wire [31:0]       dispatch_op     [0:3],
    input wire [TAG_W-1:0]  dispatch_dst    [0:3],
    // Src1
    input wire [DATA_W-1:0] dispatch_v1     [0:3],
    input wire [TAG_W-1:0]  dispatch_q1     [0:3],
    input wire              dispatch_r1     [0:3],
    // Src2
    input wire [DATA_W-1:0] dispatch_v2     [0:3],
    input wire [TAG_W-1:0]  dispatch_q2     [0:3],
    input wire              dispatch_r2     [0:3],

    output wire             issue_ready,   // 给流水线前端：RS满了，停！
    
    // 来自 CDB 的广播 (给 RS 监听用)
    input wire [3:0]        cdb_valid,
    input wire [TAG_W-1:0]  cdb_tag   [0:3],
    input wire [DATA_W-1:0] cdb_val   [0:3],

    // ALU 0 接口
    output wire             alu0_en,
    output wire [31:0]      alu0_op,
    output wire [DATA_W-1:0] alu0_v1,
    output wire [DATA_W-1:0] alu0_v2,
    output wire [TAG_W-1:0]  alu0_dst,
    
    // ALU 1 接口
    output wire             alu1_en,
    output wire [31:0]      alu1_op,
    output wire [DATA_W-1:0] alu1_v1,
    output wire [DATA_W-1:0] alu1_v2,
    output wire [TAG_W-1:0]  alu1_dst
);
    wire full_stall; 
    assign issue_ready = ~full_stall;
    // A. Allocator <-> RS 之间的控制线
    wire [RS_DEPTH-1:0]      rs_busy_wires;      // RS -> Alloc
    wire [RS_DEPTH-1:0]      alloc_wen;          // Alloc -> RS (写使能)
    wire [$clog2(RS_DEPTH)-1:0] routing_idx [0:3]; // Alloc -> Crossbar (路由地址)

    // B. RS <-> Select Logic 之间的握手线
    wire [RS_DEPTH-1:0]      rs_ready_wires;     // RS -> Select
    wire [RS_DEPTH-1:0]      grant_mask_wires;   // Select -> RS (清除Busy)

    // C. Select Logic -> ALU Mux 的选择信号
    wire [$clog2(RS_DEPTH)-1:0] alu0_sel;
    wire [$clog2(RS_DEPTH)-1:0] alu1_sel;

    // D. Crossbar <-> RS 输入数据线 (16组宽总线)
    // 这些是在 always_comb 里被驱动的
    logic [31:0]       rs_in_op  [0:RS_DEPTH-1];
    logic [TAG_W-1:0]  rs_in_dst [0:RS_DEPTH-1];
    logic [DATA_W-1:0] rs_in_v1  [0:RS_DEPTH-1];
    logic [TAG_W-1:0]  rs_in_q1  [0:RS_DEPTH-1];
    logic              rs_in_r1  [0:RS_DEPTH-1];
    logic [DATA_W-1:0] rs_in_v2  [0:RS_DEPTH-1];
    logic [TAG_W-1:0]  rs_in_q2  [0:RS_DEPTH-1];
    logic              rs_in_r2  [0:RS_DEPTH-1];

    // ==========================================
    // 模块 1: 分配器 (Allocator)
    // ==========================================
    rs_allocator #(
        .Cfg(Cfg)
    ) u_alloc (
        .rs_busy     ( rs_busy_wires ),
        .instr_valid ( dispatch_valid ),
        .entry_wen   ( alloc_wen ),
        .idx_map     ( routing_idx ),
        .full_stall  ( full_stall )
    );
    
    always_comb begin
        for (int k = 0; k < RS_DEPTH; k++) begin
            rs_in_op[k]  = 0; rs_in_dst[k] = 0;
            rs_in_v1[k]  = 0; rs_in_q1[k]  = 0; rs_in_r1[k] = 0;
            rs_in_v2[k]  = 0; rs_in_q2[k]  = 0; rs_in_r2[k] = 0;
        end

        for (int i = 0; i < 4; i++) begin
            if (dispatch_valid[i]) begin
                rs_in_op [ routing_idx[i] ] = dispatch_op[i];
                rs_in_dst[ routing_idx[i] ] = dispatch_dst[i];
                rs_in_v1 [ routing_idx[i] ] = dispatch_v1[i];
                rs_in_q1 [ routing_idx[i] ] = dispatch_q1[i];
                rs_in_r1 [ routing_idx[i] ] = dispatch_r1[i];
                rs_in_v2 [ routing_idx[i] ] = dispatch_v2[i];
                rs_in_q2 [ routing_idx[i] ] = dispatch_q2[i];
                rs_in_r2 [ routing_idx[i] ] = dispatch_r2[i];
            end
        end
    end

    reservation_station #(
        .Cfg(Cfg)
    ) u_rs (
        .clk         ( clk ),
        .rst_n       ( rst_n ),
        
        // 写端口 (连接 Crossbar 的结果)
        .entry_wen   ( alloc_wen ),
        .in_op       ( rs_in_op ),
        .in_dst_tag  ( rs_in_dst ),
        .in_v1       ( rs_in_v1 ), .in_q1(rs_in_q1), .in_r1(rs_in_r1),
        .in_v2       ( rs_in_v2 ), .in_q2(rs_in_q2), .in_r2(rs_in_r2),

        // CDB 监听端口
        .cdb_valid   ( cdb_valid ),
        .cdb_tag     ( cdb_tag ),
        .cdb_value   ( cdb_val ),

        .busy_vector ( rs_busy_wires ),

        // 状态输出
        .ready_mask  ( rs_ready_wires ),
        .issue_grant ( grant_mask_wires ),
        
        .sel_idx_0   ( alu0_sel ),
        .out_op_0    ( alu0_op ),
        .out_v1_0    ( alu0_v1 ),
        .out_v2_0    ( alu0_v2 ),
        .out_dst_tag_0 ( alu0_dst ),

        .sel_idx_1   ( alu1_sel ),
        .out_op_1    ( alu1_op ),
        .out_v1_1    ( alu1_v1 ),
        .out_v2_1    ( alu1_v2 ),
        .out_dst_tag_1 ( alu1_dst )
    );

    // ==========================================
    // 模块 3: 选择逻辑 (Select Logic)
    // ==========================================
    select_logic #(
        .Cfg(Cfg)
    ) u_select (
        .ready_mask       ( rs_ready_wires ),   
        .issue_grant_mask ( grant_mask_wires ),
        
        // 控制 ALU 0
        .alu0_valid       ( alu0_en ),
        .alu0_rs_idx      ( alu0_sel ),
        
        // 控制 ALU 1
        .alu1_valid       ( alu1_en ),
        .alu1_rs_idx      ( alu1_sel )
    );

endmodule