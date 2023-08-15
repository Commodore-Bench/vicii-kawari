// This file is part of the vicii-kawari distribution
// (https://github.com/randyrossi/vicii-kawari)
// Copyright (c) 2022 Randy Rossi.
// 
// This program is free software: you can redistribute it and/or modify  
// it under the terms of the GNU General Public License as published by  
// the Free Software Foundation, version 3.
//
// This program is distributed in the hope that it will be useful, but 
// WITHOUT ANY WARRANTY; without even the implied warranty of 
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU 
// General Public License for more details.
//
// You should have received a copy of the GNU General Public License 
// along with this program. If not, see <http://www.gnu.org/licenses/>.

`include "common.vh"

// A module to produce horizontal and vertical sync pulses for VGA/HDMI output.
// Timings are not standard and may not work on all monitors.

// This version of sync/pixel generator uses slower clocks than the full
// clk_dot4x. We can use a slower clock but get the same vertical refresh
// rate by reducing the horizontal width.  This chart outlines the changes:
//
// standard |clk_dot4x | orig_width   | clk_dvi   | dvi_width | xoffset | frac
// ----------------------------------------------------------------------------
// 6567R8   |32.727272 | 1040 (520x2) | 26.590909 | 845       | 136     | 13/16
// 6567R56A |32.727272 | 1024 (512x2) | 26.590909 | 832       | 142     | 13/16
// 6569     |31.527955 | 1008 (504x2) | 29.557458 | 945       | 46      | 15/16
//
// The benefit is the DVI 10x pixel clock does not have to be as high
// and we get better sync/pixel stability.  Also, we can chop off some
// of the border area that is not normally visible anyway.
//
// NOTE: This module cannot be compiled without HIRES_MODES enabled. The
// widths for two of the 3 chips are not evenly divisible by 2. Turning on 1x
// in the CONFIG will make no difference. A separate version of dvi sync must
// be created with 1/2 the pixel clocks shown above and then the full native
// 1x resolution can be supported. This requires alternate clock mult/div
// settings to get the 16.36mhz and 15.76mhz clocks. If used, no hires modes
// will work. There seems to be no advantage do doing this though and will
// look fuzzy on monitors as they scale up a small image.  It seems better to
// always double the horizontal width, at least for DVI output.

// A block ram module to hold a single raster line of pixel colors
module dvi_linebuf_RAM
       #(
           parameter addr_width = 11, // covers max width of 520
           data_width = 4   // 4 bit color index
       )
       (
           input wire [data_width-1:0] din,
           input wire [addr_width-1:0] addr,
           input wire rclk,
           input wire re,
           input wire wclk,
           input wire we,
           output reg [data_width-1:0] dout
       );

`ifdef WITH_64K
(* ram_style = "distributed" *) reg [data_width-1:0] ram_single_port[2**addr_width-1:0];
`else
(* ram_style = "block" *) reg [data_width-1:0] ram_single_port[2**addr_width-1:0];
`endif

always @(posedge wclk)
begin
    if (we)
        ram_single_port[addr] <= din;
end

always @(posedge rclk)
begin
    if (re)
       dout <= ram_single_port[addr];
end

endmodule

    // This module manages a double buffer of raster lines. It stores the current
    // pixel_color3 value into one buffer using raster_x as the address.  It reads
    // pixels from the other buffer using h_count as the address.  h_count increments
    // at 2x the rate of raster_x but each line is 'drawn' twice.  So v_count also
    // increments at 2x the rate as raster_y.  We are always one raster line behind.
    // The buffers are swapped after drawing a single input raster line so that we
    // are always filling one buffer while reading from the other.
    module hires_dvi_sync(
        input wire clk_dot4x,
        input wire clk_dvi,
        input [3:0] dot_rising,
        input is_native_y_in,
        input is_native_x_in,
        input hpolarity,
        input vpolarity,
        input enable_csync,
`ifdef CONFIGURABLE_TIMING
        input timing_change_in,
        input [7:0] timing_h_blank_ntsc,
        input [7:0] timing_h_fporch_ntsc,
        input [7:0] timing_h_sync_ntsc,
        input [7:0] timing_h_bporch_ntsc,
        input [7:0] timing_v_blank_ntsc,
        input [7:0] timing_v_fporch_ntsc,
        input [7:0] timing_v_sync_ntsc,
        input [7:0] timing_v_bporch_ntsc,
        input [7:0] timing_h_blank_pal,
        input [7:0] timing_h_fporch_pal,
        input [7:0] timing_h_sync_pal,
        input [7:0] timing_h_bporch_pal,
        input [7:0] timing_v_blank_pal,
        input [7:0] timing_v_fporch_pal,
        input [7:0] timing_v_sync_pal,
        input [7:0] timing_v_bporch_pal,
`endif
        input wire rst,
        input wire rst_dvi,
        input [1:0] chip,
        input [9:0] raster_x, // native res counters
        input [8:0] raster_y, // native res counters
        input [9:0] xpos,
        input [10:0] hires_raster_x,
        input [3:0] pixel_color3,
        output reg hsync,             // horizontal sync
        output reg vsync,             // vertical sync
        output reg active,
        output reg [3:0] pixel_color4,
        output reg half_bright
    );

reg [10:0] max_width; // compared to hcount which can be 2x
reg [9:0] max_height; // compared to vcount which can be 2y
reg [10:0] x_offset;

reg [10:0] hs_sta; // compared against hcount which can be 2x
reg [10:0] hs_end; // compared against hcount which can be 2x
reg [10:0] ha_sta; // compared against hcount which can be 2x
reg [10:0] ha_end; // compared against hcount which can be 2x
reg [9:0] vs_sta; // compared against vcount which can be 2y
reg [9:0] vs_end; // compared against vcount which can be 2y
reg [9:0] va_sta; // compared against vcount which can be 2y
reg [9:0] va_end; // compared against vcount which can be 2y

reg [10:0] h_count;  // output x position
reg [9:0] v_count;  // output y position
reg [1:0] ff;

wire hsync_ah;
wire vsync_ah;
wire csync_ah;

wire hsync_int;
wire vsync_int;
wire csync_int;

// Active high
assign hsync_ah = h_count >= hs_sta & h_count < hs_end;
assign vsync_ah = ((v_count == vs_end & h_count < hs_end) | v_count < vs_end) &
                  ((v_count == vs_sta & h_count >= hs_sta) | v_count > vs_sta);
assign csync_ah = hsync_ah | vsync_ah;

// Turn to active low if poliarity flag says so
assign hsync_int = hpolarity ? hsync_ah : ~hsync_ah;
assign vsync_int = vpolarity ? vsync_ah : ~vsync_ah;
assign csync_int = hpolarity ? csync_ah : ~csync_ah;

// Assign hsync/vsync or combine to csync if needed
always @ (posedge clk_dvi)
begin
   hsync <= enable_csync ? csync_int : hsync_int;
   vsync <= enable_csync ? 1'b0 : vsync_int;
end

// Note this is inverted so inclusive/exclusive comparisons swap
// around for va_end and va_sta
wire vactive;
assign vactive =
               (v_count >= va_end) &
               ((v_count == va_sta & h_count < ha_sta) | v_count < va_sta);

// active: high during active pixel drawing
always @ (posedge clk_dvi)
begin
   active <= ~(
              (h_count > ha_end | h_count <= ha_sta) |
              (vactive)
              );
end

// These conditions determine whether we advance our h/v counts
// based whether we are doubling X/Y resolutions or not.  See
// the table below for more info.
wire advance;
assign advance = !is_native_y_in ||
       (is_native_y_in && (ff == 2'b01 || ff == 2'b11));

always @ (posedge clk_dvi)
begin
    if (rst_dvi)
    begin
        ff <= 2'b01;
    end else begin
        // Resolution | advance on counter | pixel clock       | case
        // -------------------------------------------------------------------------
        // 2xX & 2xY  | 0,1,2,3            | 4x DIV 1          | !native_y && !native_x
        // 2xX & 1xY  | 1,3                | 4x DIV 2          | native_y && !native_x
        // 1xX & 2xY  | 1,3                | 4x DIV 2          | !native_y && native_x
        // 1xX & 1xY  | 1                  | 4x DIV 4 (native) | native_y & native_x
        ff <= ff + 2'b1;
        if (advance) begin
            if (h_count < max_width) begin
                h_count <= h_count + 11'b1;
            end else begin
                h_count <= 0;

                if (v_count < max_height) begin
                    v_count <= v_count + 9'b1;
                    half_bright <= ~half_bright;
                end else begin
                    v_count <= 0;
                    half_bright <= 0;
                end
            end
        end
    end
end

// Double buffer flip flop.  When active_buf is HIGH, we are writing to
// line_buf_0 while reading from line_buf_1.  Reversed when active_buf is LOW.
reg active_buf;

// Cover the max possible here. Not all may be used depending on chip.
wire [3:0] dout0; // output color from line_buf_0
wire [3:0] dout1; // output color from line_buf_1

reg [9:0] raster_x_dvi_1;
reg [9:0] raster_x_dvi;
always @(posedge clk_dvi) raster_x_dvi_1 <= raster_x;
always @(posedge clk_dvi) raster_x_dvi <= raster_x_dvi_1;
reg [8:0] raster_y_dvi_1;
reg [8:0] raster_y_dvi;
always @(posedge clk_dvi) raster_y_dvi_1 <= raster_y;
always @(posedge clk_dvi) raster_y_dvi <= raster_y_dvi_1;

// When the line buffer is being written to, we use raster_x (the VIC native
// resolution x) as the address.
// When the line buffer is being read from, we use h_count.
dvi_linebuf_RAM line_buf_0(pixel_color3, // din
                       active_buf ? hires_raster_x : (h_count + x_offset),  // addr
                       clk_dvi, // rclk
                       !active_buf, // re
                       clk_dot4x, // wclk
                       active_buf, // we
                       dout0); // dout

dvi_linebuf_RAM line_buf_1(pixel_color3,
                       !active_buf ? hires_raster_x : (h_count + x_offset),
                       clk_dvi,
                       active_buf,
                       clk_dot4x,
                       !active_buf,
                       dout1);

// Whenever we reach the beginning of a raster line, swap buffers.

always @(posedge clk_dot4x)
begin
    if (!rst) begin
        if (dot_rising[1]) begin
            if (raster_x == 0)
                active_buf = ~active_buf;
        end
    end
end

reg active_buf_1;
reg active_buf_2;
always @(posedge clk_dvi) active_buf_1 <= active_buf;
always @(posedge clk_dvi) active_buf_2 <= active_buf_1;
always @(posedge clk_dvi) begin
    pixel_color4 <= active_buf_2 ? dout1 : dout0;
end

`ifndef CONFIGURABLE_TIMING
`ifdef SIMULATOR_BOARD
always @(posedge clk_dvi)
`else
always @(chip, is_native_y_in)
`endif
    begin
        case (chip)
            `CHIP6569R1, `CHIP6569R3: begin
                // WIDTH 945  HEIGHT 312(624)
                ha_end=11'd936; // start 936 - always 2x
                hs_sta=11'd0;  // fporch  9 - always 2x
                hs_end=11'd128;  // sync  128 - always 2x
                ha_sta=11'd136;  // bporch  8 - always 2x
                va_end=10'd300;  // start 300
                vs_sta=10'd301;  // fporch   1
                vs_end=10'd309;  // sync   8
                va_sta=10'd310;  // bporch   1
                max_height = is_native_y_in ? 10'd311 : 10'd623;
                max_width = 11'd944;
                x_offset = 11'd46;
            end
            `CHIP6567R8: begin
                // WIDTH 845  HEIGHT 263(526)
                ha_end=11'd842; // start 842 - always 2x
                hs_sta=11'd0;  // fporch 3 - always 2x
                hs_end=11'd64;  // sync 64 - always 2x
                ha_sta=11'd66;  // bporch  2 - always 2x
                va_end=10'd13;  // start  13
                vs_sta=10'd14;  // fporch   1
                vs_end=10'd22;  // sync   8
                va_sta=10'd23;  // bporch   1
                max_height = is_native_y_in ? 10'd262 : 10'd525;
                max_width = 11'd844;
                x_offset = 11'd136;
            end
            `CHIP6567R56A: begin
                // WIDTH 832  HEIGHT 262(524)
                ha_end=11'd828;  // start 829 - always 2x
                hs_sta=11'd0;  // fporch  4 - always 2x
                hs_end=11'd64;  // sync  64 - always 2x
                ha_sta=11'd66;  // bporch  2 - always 2x
                va_end=10'd13;  // start  13
                vs_sta=10'd14;  // fporch   1
                vs_end=10'd22;  // sync   8
                va_sta=10'd23;  // bporch   1
                max_height = is_native_y_in ? 10'd261 : 10'd523;
                max_width = 11'd831;
                x_offset = 11'd142;
            end
        endcase

        // h* values already doubled as there is no native x for this module

        if (!is_native_y_in) begin
            va_end = { va_end[8:0], 1'b0 };
            vs_sta = { vs_sta[8:0], 1'b0 };
            vs_end = { vs_end[8:0], 1'b0 };
            va_sta = { va_sta[8:0], 1'b0 };
        end
    end
`else
// There are restrictions on the values that can be placed into the timing
// registers that the TIMED program will not enforce.  For example, any
// hs_sta value must end up being>=0. Otherwise, the range comparison
// for hsync will not make sense.
// Ranges must never straddle 0 for any chip.
always @(chip, is_native_y_in)
    begin
        case (chip)
            `CHIP6569R1, `CHIP6569R3: begin
                ha_end = {3'b000, timing_h_blank_pal} + 11'd768;
                hs_sta = ha_end + {3'b000, timing_h_fporch_pal};
                if (hs_sta >= 11'd945) hs_sta = hs_sta - 11'd945;
                hs_end = hs_sta + {3'b000, timing_h_sync_pal};
                if (hs_end >= 11'd945) hs_end = hs_end - 11'd945;
                ha_sta = hs_end + {3'b000, timing_h_bporch_pal};
                if (ha_sta >= 11'd945) ha_sta = ha_sta - 11'd945;
                // WIDTH 945
                va_end = {2'b01, timing_v_blank_pal}; // starts at 256
                vs_sta = va_end + {2'b00, timing_v_fporch_pal};
                vs_end = vs_sta + {2'b00, timing_v_sync_pal};
                va_sta = vs_end + {2'b00, timing_v_bporch_pal};
                // HEIGHT 312(624)
                max_height = is_native_y_in ? 10'd311 : 10'd623;
                max_width = 11'd944;
                x_offset = 11'd46;
            end
            `CHIP6567R8: begin
                ha_end = {3'b000, timing_h_blank_ntsc} + 11'd768;
                hs_sta = ha_end + {3'b000, timing_h_fporch_ntsc};
                if (hs_sta >= 11'd845) hs_sta = hs_sta - 11'd845;
                hs_end = hs_sta + {3'b000, timing_h_sync_ntsc};
                if (hs_end >= 11'd845) hs_end = hs_end - 11'd845;
                ha_sta = hs_end + {3'b000, timing_h_bporch_ntsc};
                if (ha_sta >= 11'd845) ha_sta = ha_sta - 11'd845;
                // WIDTH 845
                va_end = {2'b00, timing_v_blank_ntsc};
                vs_sta = va_end + {2'b00, timing_v_fporch_ntsc};
                vs_end = vs_sta + {2'b00, timing_v_sync_ntsc};
                va_sta = vs_end + {2'b00, timing_v_bporch_ntsc};
                // HEIGHT 263(526)
                max_height = is_native_y_in ? 10'd262 : 10'd525;
                max_width = 11'd844;
                x_offset = 11'd136;
            end
            `CHIP6567R56A: begin
                ha_end = {3'b000, timing_h_blank_ntsc} + 11'd768;
                hs_sta = ha_end + {3'b000, timing_h_fporch_ntsc};
                if (hs_sta >= 11'd832) hs_sta = hs_sta - 11'd832;
                hs_end = hs_sta + {3'b000, timing_h_sync_ntsc};
                if (hs_end >= 11'd832) hs_end = hs_end - 11'd832;
                ha_sta = hs_end + {3'b000, timing_h_bporch_ntsc};
                if (ha_sta >= 11'd832) ha_sta = ha_sta - 11'd832;
                // WIDTH 832
                va_end = {2'b00, timing_v_blank_ntsc};
                vs_sta = va_end + {2'b00, timing_v_fporch_ntsc};
                vs_end = vs_sta + {2'b00, timing_v_sync_ntsc};
                va_sta = vs_end + {2'b00, timing_v_bporch_ntsc};
                // HEIGHT 262(524)
                max_height = is_native_y_in ? 10'd261 : 10'd523;
                max_width = 11'd831;
                x_offset = 11'd142;
            end
        endcase

        if (!is_native_y_in) begin
            va_end = { va_end[8:0], 1'b0 };
            vs_sta = { vs_sta[8:0], 1'b0 };
            vs_end = { vs_end[8:0], 1'b0 };
            va_sta = { va_sta[8:0], 1'b0 };
        end
    end
`endif

endmodule
