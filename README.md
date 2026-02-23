# Tidy Tuples and Flying Start

**Vibe-Coded**

Just to get a sense of how the Umbra paper works, I gave the paper text to claude and asked it to create a working Proof of Concept.

Has the following things:

- Has a toy SQL parser, Planner
- Tidy Tuples Code Generator
- Umbra IR
- Flying Start x86-64 assembly emitter via asmJIT
- EXPLAIN syntax which can print all the stages
- A Repl

Example output:

```asm
experiments/umbra-v0 - [master] » bin/umbra
╔═══════════════════════════════════════════════════════╗
║  Umbra v0  —  Tidy Tuples + Umbra IR + Flying Start
║  Based on: Kersten, Leis, Neumann — PVLDB 2021
╚═══════════════════════════════════════════════════════╝

  Type .help for commands, .tables to list tables.

umbra> .load data/customers.csv c
  Loaded c: 10000 rows, 4 cols  (8.9 ms)
  Columns: id, name(str), city(str), credit_limit

umbra> .load data/orders.csv o
  Loaded o: 50000 rows, 4 cols  (23.6 ms)
  Columns: id, customer_id, amount, status(str)

umbra> EXPLAIN tt=1 fs=1 SELECT * FROM o, c WHERE o.customer_id = c.id LIMIT 10;

── Operator Tree (Tidy Tuples Layer 1: produce/consume) ────────
  HashJoin  [1 key(s)]
    ├─ Scan(o)  [4 cols]
    └─ Scan(c)  [4 cols]

  Output columns: id, customer_id, amount, status, id, name, city, credit_limit

── Umbra IR  (Tidy Tuples → IR, after DCE) ─────────────────────
── instrData: 1276 bytes, 65 instructions (DCE removed 2)

define void sql_query() {
block0 [entry]:
  %0 = const i64 0
  %16 = const i64 50000
  %32 = const i64 1
  br block1
block1 [o_scan_header]:
  %56 = phi i64 [%0:block0, %508:block2]
  %80 = cmplt i1 %56, %16
  condbr %80, block2, block3
block2 [o_scan_body]:
  %108 = const ptr 45890125824
  %124 = call i64 @0x10259dcf4(%108, %56)
  %156 = const ptr 45893402624
  %172 = call i64 @0x10259dcf4(%156, %56)
  %204 = const ptr 45890650112
  %220 = call i64 @0x10259dcf4(%204, %56)
  %252 = const ptr 45891059712
  %268 = call i64 @0x10259dcf4(%252, %56)
  %300 = const u64 6763793487589347598
  %316 = const u64 4593845798347983834
  %344 = zext u64 %172
  %352 = crc32 u64 %300, %344
  %364 = crc32 u64 %316, %344
  %376 = rotr u64 %364, 32
  %388 = xor u64 %352, %376
  %400 = const u64 -7046029254386353131
  %416 = mul u64 %388, %400
  %428 = const ptr 4340288672
  %444 = call void @0x10259feec(%428, %416, %172, %0, %0, %0, %124, %220, %268, %0)
  %508 = add i64 %56, %32
  br block1
block3 [o_scan_exit]:
  %528 = const i64 10000
  br block4
block4 [c_scan_header]:
  %552 = phi i64 [%0:block3, %1248:block9]
  %576 = cmplt i1 %552, %528
  condbr %576, block5, block6
block5 [c_scan_body]:
  %604 = const ptr 45889880064
  %620 = call i64 @0x10259dcf4(%604, %552)
  %652 = const ptr 45889961984
  %668 = call i64 @0x10259dcf4(%652, %552)
  %700 = const ptr 45890043904
  %716 = call i64 @0x10259dcf4(%700, %552)
  %748 = const ptr 45890568192
  %764 = call i64 @0x10259dcf4(%748, %552)
  %808 = zext u64 %620
  %816 = crc32 u64 %300, %808
  %828 = crc32 u64 %316, %808
  %840 = rotr u64 %828, 32
  %852 = xor u64 %816, %840
  %864 = mul u64 %852, %400
  %876 = call ptr @0x10259ff54(%428, %864, %620, %0, %0, %0)
  %924 = const ptr 0
  %940 = cmpne i1 %876, %924
  condbr %940, block7, block8
block6 [c_scan_exit]:
  ret void
block7 [if_true]:
  %968 = const i32 0
  %984 = call i64 @0x1025a0018(%876, %968)
  %1016 = call i64 @0x1025a000c(%876, %968)
  %1048 = const i32 1
  %1064 = call i64 @0x1025a000c(%876, %1048)
  %1096 = const i32 2
  %1112 = call i64 @0x1025a000c(%876, %1096)
  %1144 = const ptr 6132535408
  %1160 = const i64 8
  %1176 = call void @0x1025ab9f8(%1144, %1160, %1016, %984, %1064, %1112, %620, %668, %716, %764)
  br block9
block8 [if_false]:
block9 [if_merge]:
  %1248 = add i64 %552, %32
  br block4
}


── Flying Start  (Umbra IR → native code via asmJIT) ───────────
.section .text {#0}
L1:
stp x19, x20, [sp, -64]!                    ; F353BCA9
stp x21, x22, [sp, 16]                      ; F55B01A9
stp x23, x24, [sp, 32]                      ; F76302A9
str x30, [sp, 48]                           ; FE1B00F9
sub sp, sp, 0x80                            ; FF0302D1
L2:
mov x0, 0                                   ; 000080D2
mov x10, 0xC350                             ; 0A6A98D2
mov x16, 1                                  ; 300080D2
mov x0, 0                                   ; 000080D2
str x0, [sp, 72]                            ; E02700F9
b L3                                        ; 00000014
L3:
ldr x0, [sp, 72]                            ; E02740F9
cmp x0, x10                                 ; 1F000AEB
b.lt L4                                     ; 0B000054
b L5                                        ; 00000014
L4:
mov x0, 0xAAF43C000                         ; 000098D260E8B5F24001C0F2
mov x0, 0xAAF43C000                         ; 000098D260E8B5F24001C0F2
mov x2, 0x10259DCF4                         ; 829E9BD2224BA0F22200C0F2
ldr x1, [sp, 72]                            ; E12740F9
str x10, [sp, 104]                          ; EA3700F9
str x16, [sp, 96]                           ; F03300F9
blr x2                                      ; 40003FD6
mov x1, 0xAAF75C000                         ; 010098D2A1EEB5F24101C0F2
mov x1, 0xAAF75C000                         ; 010098D2A1EEB5F24101C0F2
mov x2, 0x10259DCF4                         ; 829E9BD2224BA0F22200C0F2
mov x19, x0                                 ; F30300AA
mov x20, x1                                 ; F40301AA
mov x0, x20                                 ; E00314AA
ldr x1, [sp, 72]                            ; E12740F9
blr x2                                      ; 40003FD6
mov x1, 0xAAF4BC000                         ; 010098D261E9B5F24101C0F2
mov x1, 0xAAF4BC000                         ; 010098D261E9B5F24101C0F2
mov x2, 0x10259DCF4                         ; 829E9BD2224BA0F22200C0F2
mov x20, x0                                 ; F40300AA
mov x21, x1                                 ; F50301AA
mov x0, x21                                 ; E00315AA
ldr x1, [sp, 72]                            ; E12740F9
blr x2                                      ; 40003FD6
mov x1, 0xAAF520000                         ; 41EAB5D24101C0F2
mov x1, 0xAAF520000                         ; 41EAB5D24101C0F2
mov x2, 0x10259DCF4                         ; 829E9BD2224BA0F22200C0F2
mov x21, x0                                 ; F50300AA
mov x22, x1                                 ; F60301AA
mov x0, x22                                 ; E00316AA
ldr x1, [sp, 72]                            ; E12740F9
blr x2                                      ; 40003FD6
mov x14, 0x5DDDD25741DD110E                 ; CE2182D2AE3BA8F2EE4ADAF2AEBBEBF2
mov x13, 0x3FC09E6A1373CBDA                 ; 4D7B99D26D6EA2F24DCDD3F20DF8E7F2
mov x1, x20                                 ; E10314AA
crc32cx w2, w14, x1                         ; C25DC19A
crc32cx w1, w13, x1                         ; A15DC19A
ror x1, x1, 0x20                            ; 2180C193
eor x1, x2, x1                              ; 410001CA
mov x12, 0x9E3779B97F4A7C15                 ; AC828FD24CE9AFF22C37CFF2ECC6F3F2
mul x1, x1, x12                             ; 217C0C9B
mov x2, 0x102B38CA0                         ; 029491D26256A0F22200C0F2
mov x2, 0x102B38CA0                         ; 029491D26256A0F22200C0F2
mov x3, 0                                   ; 030080D2
mov x4, 0                                   ; 040080D2
mov x5, 0                                   ; 050080D2
mov x6, 0                                   ; 060080D2
mov x8, 0x10259FEEC                         ; 88DD9FD2284BA0F22800C0F2
str x0, [sp]                                ; E00300F9
str x6, [sp, 8]                             ; E60700F9
mov x22, x2                                 ; F60302AA
mov x0, x22                                 ; E00316AA
mov x2, x20                                 ; E20314AA
mov x6, x19                                 ; E60313AA
mov x7, x21                                 ; E70315AA
str x12, [sp, 88]                           ; EC2F00F9
str x13, [sp, 80]                           ; ED2B00F9
str x14, [sp, 64]                           ; EE2300F9
blr x8                                      ; 00013FD6
ldr x0, [sp, 72]                            ; E02740F9
ldr x16, [sp, 96]                           ; F03340F9
add x0, x0, x16                             ; 0000108B
mov x0, x0                                  ; E00300AA
str x0, [sp, 72]                            ; E02700F9
ldr x10, [sp, 104]                          ; EA3740F9
b L3                                        ; 97FFFF17
L5:
mov x15, 0x2710                             ; 0FE284D2
mov x0, 0                                   ; 000080D2
str x0, [sp, 24]                            ; E00F00F9
b L6                                        ; 00000014
L6:
ldr x0, [sp, 24]                            ; E00F40F9
cmp x0, x15                                 ; 1F000FEB
b.lt L7                                     ; 0B000054
b L8                                        ; 00000014
L7:
mov x0, 0xAAF400000                         ; 00E8B5D24001C0F2
mov x0, 0xAAF400000                         ; 00E8B5D24001C0F2
mov x2, 0x10259DCF4                         ; 829E9BD2224BA0F22200C0F2
ldr x1, [sp, 24]                            ; E10F40F9
str x15, [sp, 16]                           ; EF0B00F9
str x16, [sp, 96]                           ; F03300F9
blr x2                                      ; 40003FD6
mov x1, 0xAAF414000                         ; 010088D221E8B5F24101C0F2
mov x1, 0xAAF414000                         ; 010088D221E8B5F24101C0F2
mov x2, 0x10259DCF4                         ; 829E9BD2224BA0F22200C0F2
str x0, [sp, 56]                            ; E01F00F9
mov x19, x1                                 ; F30301AA
mov x0, x19                                 ; E00313AA
ldr x1, [sp, 24]                            ; E10F40F9
blr x2                                      ; 40003FD6
mov x1, 0xAAF428000                         ; 010090D241E8B5F24101C0F2
mov x1, 0xAAF428000                         ; 010090D241E8B5F24101C0F2
mov x2, 0x10259DCF4                         ; 829E9BD2224BA0F22200C0F2
str x0, [sp, 48]                            ; E01B00F9
mov x19, x1                                 ; F30301AA
mov x0, x19                                 ; E00313AA
ldr x1, [sp, 24]                            ; E10F40F9
blr x2                                      ; 40003FD6
mov x1, 0xAAF4A8000                         ; 010090D241E9B5F24101C0F2
mov x1, 0xAAF4A8000                         ; 010090D241E9B5F24101C0F2
mov x2, 0x10259DCF4                         ; 829E9BD2224BA0F22200C0F2
str x0, [sp, 40]                            ; E01700F9
mov x19, x1                                 ; F30301AA
mov x0, x19                                 ; E00313AA
ldr x1, [sp, 24]                            ; E10F40F9
blr x2                                      ; 40003FD6
ldr x7, [sp, 56]                            ; E71F40F9
mov x1, x7                                  ; E10307AA
ldr x14, [sp, 64]                           ; EE2340F9
crc32cx w2, w14, x1                         ; C25DC19A
ldr x13, [sp, 80]                           ; ED2B40F9
crc32cx w1, w13, x1                         ; A15DC19A
ror x1, x1, 0x20                            ; 2180C193
eor x1, x2, x1                              ; 410001CA
ldr x12, [sp, 88]                           ; EC2F40F9
mul x1, x1, x12                             ; 217C0C9B
mov x2, 0x102B38CA0                         ; 029491D26256A0F22200C0F2
mov x3, 0                                   ; 030080D2
mov x4, 0                                   ; 040080D2
mov x5, 0                                   ; 050080D2
mov x6, 0x10259FF54                         ; 86EA9FD2264BA0F22600C0F2
str x0, [sp, 32]                            ; E01300F9
mov x19, x2                                 ; F30302AA
mov x0, x19                                 ; E00313AA
mov x2, x7                                  ; E20307AA
blr x6                                      ; C0003FD6
mov x1, 0                                   ; 010080D2
cmp x0, x1                                  ; 1F0001EB
b.ne L9                                     ; 01000054
b L10                                       ; 00000014
L8:
b L0                                        ; 00000014
L9:
mov x1, 0                                   ; 010080D2
mov x1, 0                                   ; 010080D2
mov x2, 0x1025A0018                         ; 020380D2424BA0F22200C0F2
str x0, [sp, 112]                           ; E03B00F9
blr x2                                      ; 40003FD6
mov x1, 0                                   ; 010080D2
mov x2, 0x1025A000C                         ; 820180D2424BA0F22200C0F2
mov x19, x0                                 ; F30300AA
ldr x0, [sp, 112]                           ; E03B40F9
blr x2                                      ; 40003FD6
mov x1, 1                                   ; 210080D2
mov x1, 1                                   ; 210080D2
mov x2, 0x1025A000C                         ; 820180D2424BA0F22200C0F2
mov x20, x0                                 ; F40300AA
ldr x0, [sp, 112]                           ; E03B40F9
blr x2                                      ; 40003FD6
mov x1, 2                                   ; 410080D2
mov x1, 2                                   ; 410080D2
mov x2, 0x1025A000C                         ; 820180D2424BA0F22200C0F2
mov x21, x0                                 ; F50300AA
ldr x0, [sp, 112]                           ; E03B40F9
blr x2                                      ; 40003FD6
mov x1, 0x16D871070                         ; 010E82D2E1B0ADF22100C0F2
mov x1, 8                                   ; 010180D2
mov x1, 0x16D871070                         ; 010E82D2E1B0ADF22100C0F2
mov x2, 8                                   ; 020180D2
mov x8, 0x1025AB9F8                         ; 083F97D2484BA0F22800C0F2
ldr x10, [sp, 40]                           ; EA1740F9
str x10, [sp]                               ; EA0300F9
ldr x9, [sp, 32]                            ; E91340F9
str x9, [sp, 8]                             ; E90700F9
mov x22, x0                                 ; F60300AA
mov x23, x1                                 ; F70301AA
mov x24, x2                                 ; F80302AA
mov x0, x23                                 ; E00317AA
mov x1, x24                                 ; E10318AA
mov x2, x20                                 ; E20314AA
mov x3, x19                                 ; E30313AA
mov x4, x21                                 ; E40315AA
mov x5, x22                                 ; E50316AA
ldr x6, [sp, 56]                            ; E61F40F9
ldr x7, [sp, 48]                            ; E71B40F9
blr x8                                      ; 00013FD6
b L11                                       ; 00000014
L10:
L11:
ldr x0, [sp, 24]                            ; E00F40F9
ldr x16, [sp, 96]                           ; F03340F9
add x0, x0, x16                             ; 0000108B
mov x0, x0                                  ; E00300AA
str x0, [sp, 24]                            ; E00F00F9
ldr x15, [sp, 16]                           ; EF0B40F9
b L6                                        ; 6AFFFF17
L0:
add sp, sp, 0x80                            ; FF030291
ldr x30, [sp, 48]                           ; FE1B40F9
ldp x23, x24, [sp, 32]                      ; F76342A9
ldp x21, x22, [sp, 16]                      ; F55B41A9
ldp x19, x20, [sp], 64                      ; F353C4A8
ret x30                                     ; C0035FD6

; ── Flying Start Statistics (via asmJIT) ─────────────────
; Values allocated in registers:      51
; Comparisons fused with branches:    3
; Lazy address calculations (GEP):    0
; Mov instructions eliminated:        4
; (Stack spills handled by asmJIT RA)
```