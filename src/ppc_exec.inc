/*
 * The body of the interpreter: executes instruction `i` at `pc`, setting
 * `npc` for branches. Included by ppc.c with these defined: c, m (struct
 * mem *), g (c->gpr), i, pc, npc, done (instructions retired since c->tb
 * was updated), hook, and the macros
 *   EXIT(e)       stop with exit reason e; the instruction has changed nothing
 *   CHECK_HOOK()  after a branch has set npc
 *   IN_PLACE      whether kernel calls may be answered in place (see ppc.h)
 *   UNRETIRED()   once one is: it completes without counting as retired
 */
#define RA0(i) (RA(i) ? g[RA(i)] : 0)
#define DSI(ea, w) do { c->fault_addr = (ea); c->fault_write = (w); EXIT(PPC_EXIT_DSI); } while (0)
#define CHECK_R(ea, n) do { if (!mem_ok(m, (ea), (n), MEM_R)) DSI((ea), false); } while (0)
#define CHECK_W(ea, n) do { if (!mem_ok_w(m, (ea), (n))) DSI((ea), true); } while (0)

	switch (OPCD(i)) {
	case 3:                                                         /* twi */
		if (trap_taken(RD(i), g[RA(i)], SIMM(i))) {
			if (IN_PLACE && (i & 0xFFFF0000u) == 0x0FFF0000u && c->tb + done * c->tb_step < c->kcall_until) {
				c->pc = pc;
				if (c->kcall(c, i & 0xFFFF)) {
					UNRETIRED();
					npc = c->pc;
					break;
				}
			}
			EXIT(PPC_EXIT_TRAP);
		}
		break;
	case 6:
		EXIT(PPC_EXIT_HOSTCALL);
	case 7:                                                         /* mulli */
		g[RD(i)] = (uint32_t)((int64_t)(int32_t)g[RA(i)] * (int32_t)SIMM(i));
		break;
	case 8:                                                         /* subfic */
		g[RD(i)] = add_ext(c, ~g[RA(i)], SIMM(i), 1, true, false);
		break;
	case 10:                                                        /* cmpli */
		set_crf(c, CRFD(i), cmp_unsigned(c, g[RA(i)], UIMM(i)));
		break;
	case 11:                                                        /* cmpi */
		set_crf(c, CRFD(i), cmp_signed(c, (int32_t)g[RA(i)], (int32_t)SIMM(i)));
		break;
	case 12:                                                        /* addic */
	case 13:                                                        /* addic. */
		g[RD(i)] = add_ext(c, g[RA(i)], SIMM(i), 0, true, false);
		if (OPCD(i) == 13)
			record(c, g[RD(i)]);
		break;
	case 14:                                                        /* addi */
		g[RD(i)] = RA0(i) + SIMM(i);
		break;
	case 15:                                                        /* addis */
		g[RD(i)] = RA0(i) + (UIMM(i) << 16);
		break;
	case 16: {                                                      /* bc */
		uint32_t bd = SIMM(i & 0xFFFC);
		if (branch_ok(c, RD(i), RA(i), true))
			npc = (i & 2) ? bd : pc + bd;
		if (i & 1)
			c->lr = pc + 4;
		CHECK_HOOK();
		break;
	}
	case 17:                                                        /* sc */
		if (!IN_PLACE || (g[0] != 0xFFFFFFFFu && g[0] != 0xFFFFFFFEu) ||
		    c->tb + done * c->tb_step >= c->sc_until)
			EXIT(PPC_EXIT_SC);
		UNRETIRED();
		set_crf(c, 0, (c->sc_blue ? CR_GT : CR_EQ) | so_bit(c));
		g[0] = g[0] == 0xFFFFFFFFu ? c->sc_blue : c->sc_word;
		c->exit_insn = i;                                       /* (as the exit would leave it) */
		break;
	case 18: {                                                      /* b */
		uint32_t li = i & 0x03FFFFFCu;
		if (li & 0x02000000u)
			li |= 0xFC000000u;
		npc = (i & 2) ? li : pc + li;
		if (i & 1)
			c->lr = pc + 4;
		CHECK_HOOK();
		break;
	}
	case 19:
		switch (XO10(i)) {
		case 0:                                                 /* mcrf */
			set_crf(c, CRFD(i), c->cr >> (28 - 4 * ((i >> 18) & 7)));
			break;
		case 16: {                                              /* bclr */
			uint32_t target = c->lr & ~3u;
			if (branch_ok(c, RD(i), RA(i), true))
				npc = target;
			if (i & 1)
				c->lr = pc + 4;
			CHECK_HOOK();
			break;
		}
		case 528: {                                             /* bcctr */
			uint32_t target = c->ctr & ~3u;
			if (branch_ok(c, RD(i), RA(i), false))
				npc = target;
			if (i & 1)
				c->lr = pc + 4;
			CHECK_HOOK();
			break;
		}
		case 33: case 129: case 193: case 225:
		case 257: case 289: case 417: case 449: {
			unsigned a = (c->cr >> (31 - RA(i))) & 1;
			unsigned b = (c->cr >> (31 - RB(i))) & 1;
			unsigned r;
			switch (XO10(i)) {
			case 33:  r = !(a | b); break;                  /* crnor */
			case 129: r = a & !b; break;                    /* crandc */
			case 193: r = a ^ b; break;                     /* crxor */
			case 225: r = !(a & b); break;                  /* crnand */
			case 257: r = a & b; break;                     /* crand */
			case 289: r = !(a ^ b); break;                  /* creqv */
			case 417: r = a | !b; break;                    /* crorc */
			default:  r = a | b; break;                     /* cror */
			}
			unsigned bt = 31 - RD(i);
			c->cr = (c->cr & ~(1u << bt)) | ((uint32_t)r << bt);
			break;
		}
		case 150:                                               /* isync */
			break;
		case 50:                                                /* rfi */
			EXIT(PPC_EXIT_PRIV);
		default:
			EXIT(PPC_EXIT_ILLEGAL);
		}
		break;
	case 20: {                                                      /* rlwimi */
		uint32_t msk = mask32((i >> 6) & 31, (i >> 1) & 31);
		g[RA(i)] = (rotl32(g[RS(i)], RB(i)) & msk) | (g[RA(i)] & ~msk);
		if (RC(i))
			record(c, g[RA(i)]);
		break;
	}
	case 21:                                                        /* rlwinm */
		g[RA(i)] = rotl32(g[RS(i)], RB(i)) & mask32((i >> 6) & 31, (i >> 1) & 31);
		if (RC(i))
			record(c, g[RA(i)]);
		break;
	case 23:                                                        /* rlwnm */
		g[RA(i)] = rotl32(g[RS(i)], g[RB(i)] & 31) & mask32((i >> 6) & 31, (i >> 1) & 31);
		if (RC(i))
			record(c, g[RA(i)]);
		break;
	case 24: g[RA(i)] = g[RS(i)] | UIMM(i); break;                          /* ori */
	case 25: g[RA(i)] = g[RS(i)] | (UIMM(i) << 16); break;                  /* oris */
	case 26: g[RA(i)] = g[RS(i)] ^ UIMM(i); break;                          /* xori */
	case 27: g[RA(i)] = g[RS(i)] ^ (UIMM(i) << 16); break;                  /* xoris */
	case 28: g[RA(i)] = g[RS(i)] & UIMM(i); record(c, g[RA(i)]); break;     /* andi. */
	case 29: g[RA(i)] = g[RS(i)] & (UIMM(i) << 16); record(c, g[RA(i)]); break; /* andis. */
	case 31:
		if (exec_arith(c, i))
			break;
		switch (XO10(i)) {
		case 0:                                                 /* cmp */
			set_crf(c, CRFD(i), cmp_signed(c, (int32_t)g[RA(i)], (int32_t)g[RB(i)]));
			break;
		case 32:                                                /* cmpl */
			set_crf(c, CRFD(i), cmp_unsigned(c, g[RA(i)], g[RB(i)]));
			break;
		case 4:                                                 /* tw */
			if (trap_taken(RD(i), g[RA(i)], g[RB(i)]))
				EXIT(PPC_EXIT_TRAP);
			break;
		case 19:                                                /* mfcr */
			g[RD(i)] = c->cr;
			break;
		case 144: {                                             /* mtcrf */
			uint32_t crm = (i >> 12) & 0xFF, msk = 0;
			for (unsigned f = 0; f < 8; f++)
				if (crm & (0x80u >> f))
					msk |= 0xFu << (28 - 4 * f);
			c->cr = (c->cr & ~msk) | (g[RS(i)] & msk);
			break;
		}
		case 512:                                               /* mcrxr */
			set_crf(c, CRFD(i), c->xer >> 28);
			c->xer &= 0x0FFFFFFFu;
			break;
		case 339:                                               /* mfspr */
			switch (SPRN(i)) {
			case 1:   g[RD(i)] = c->xer; break;
			case 8:   g[RD(i)] = c->lr; break;
			case 9:   g[RD(i)] = c->ctr; break;
			case 256: g[RD(i)] = c->vrsave; break;
			case 268: g[RD(i)] = (uint32_t)(c->tb + done * c->tb_step); break;
			case 269: g[RD(i)] = (uint32_t)((c->tb + done * c->tb_step) >> 32); break;
			default:  EXIT((SPRN(i) & 0x10) ? PPC_EXIT_PRIV : PPC_EXIT_ILLEGAL);
			}
			break;
		case 467:                                               /* mtspr */
			switch (SPRN(i)) {
			case 1:   c->xer = g[RS(i)]; break;
			case 8:   c->lr = g[RS(i)]; break;
			case 9:   c->ctr = g[RS(i)]; break;
			case 256: c->vrsave = g[RS(i)]; break;
			default:  EXIT((SPRN(i) & 0x10) ? PPC_EXIT_PRIV : PPC_EXIT_ILLEGAL);
			}
			break;
		case 371:                                               /* mftb */
			switch (SPRN(i)) {
			case 268: g[RD(i)] = (uint32_t)(c->tb + done * c->tb_step); break;
			case 269: g[RD(i)] = (uint32_t)((c->tb + done * c->tb_step) >> 32); break;
			default:  EXIT(PPC_EXIT_ILLEGAL);
			}
			break;
		case 83: case 146: case 210: case 242: case 595: case 659:
		case 306: case 370: case 566: case 470:
			/* mfmsr mtmsr mtsr mtsrin mfsr mfsrin tlbie tlbia tlbsync dcbi */
			EXIT(PPC_EXIT_PRIV);
		case 26: {                                              /* cntlzw */
			uint32_t s = g[RS(i)];
			g[RA(i)] = s ? (uint32_t)__builtin_clz(s) : 32;
			if (RC(i))
				record(c, g[RA(i)]);
			break;
		}
		case 28: case 60: case 124: case 284: case 316: case 412: case 444: case 476: {
			uint32_t s = g[RS(i)], b = g[RB(i)], r;
			switch (XO10(i)) {
			case 28:  r = s & b; break;                     /* and */
			case 60:  r = s & ~b; break;                    /* andc */
			case 124: r = ~(s | b); break;                  /* nor */
			case 284: r = ~(s ^ b); break;                  /* eqv */
			case 316: r = s ^ b; break;                     /* xor */
			case 412: r = s | ~b; break;                    /* orc */
			case 444: r = s | b; break;                     /* or */
			default:  r = ~(s & b); break;                  /* nand */
			}
			g[RA(i)] = r;
			if (RC(i))
				record(c, r);
			break;
		}
		case 24: case 536: {                                    /* slw, srw */
			uint32_t n = g[RB(i)] & 0x3F, s = g[RS(i)];
			g[RA(i)] = (n & 0x20) ? 0 : XO10(i) == 24 ? s << n : s >> n;
			if (RC(i))
				record(c, g[RA(i)]);
			break;
		}
		case 792: case 824: {                                   /* sraw, srawi */
			uint32_t n = XO10(i) == 824 ? RB(i) : g[RB(i)] & 0x3F;
			uint32_t s = g[RS(i)];
			bool neg = s & 0x80000000u;
			if (n & 0x20) {
				g[RA(i)] = neg ? 0xFFFFFFFFu : 0;
				set_ca(c, neg);
			} else {
				g[RA(i)] = sra32(s, n);
				set_ca(c, neg && n && (s & ((1u << n) - 1)));
			}
			if (RC(i))
				record(c, g[RA(i)]);
			break;
		}
		case 922:                                               /* extsh */
			g[RA(i)] = (uint32_t)(int32_t)(int16_t)g[RS(i)];
			if (RC(i))
				record(c, g[RA(i)]);
			break;
		case 954:                                               /* extsb */
			g[RA(i)] = (uint32_t)(int32_t)(int8_t)g[RS(i)];
			if (RC(i))
				record(c, g[RA(i)]);
			break;

		/* indexed loads and stores */
		case 23: case 55: {                                     /* lwzx, lwzux */
			uint32_t ea = (XO10(i) == 55 ? g[RA(i)] : RA0(i)) + g[RB(i)];
			CHECK_R(ea, 4);
			g[RD(i)] = mem_r32(m, ea);
			if (XO10(i) == 55)
				g[RA(i)] = ea;
			break;
		}
		case 87: case 119: {                                    /* lbzx, lbzux */
			uint32_t ea = (XO10(i) == 119 ? g[RA(i)] : RA0(i)) + g[RB(i)];
			CHECK_R(ea, 1);
			g[RD(i)] = mem_r8(m, ea);
			if (XO10(i) == 119)
				g[RA(i)] = ea;
			break;
		}
		case 279: case 311: {                                   /* lhzx, lhzux */
			uint32_t ea = (XO10(i) == 311 ? g[RA(i)] : RA0(i)) + g[RB(i)];
			CHECK_R(ea, 2);
			g[RD(i)] = mem_r16(m, ea);
			if (XO10(i) == 311)
				g[RA(i)] = ea;
			break;
		}
		case 343: case 375: {                                   /* lhax, lhaux */
			uint32_t ea = (XO10(i) == 375 ? g[RA(i)] : RA0(i)) + g[RB(i)];
			CHECK_R(ea, 2);
			g[RD(i)] = (uint32_t)(int32_t)(int16_t)mem_r16(m, ea);
			if (XO10(i) == 375)
				g[RA(i)] = ea;
			break;
		}
		case 151: case 183: {                                   /* stwx, stwux */
			uint32_t ea = (XO10(i) == 183 ? g[RA(i)] : RA0(i)) + g[RB(i)];
			CHECK_W(ea, 4);
			mem_w32(m, ea, g[RS(i)]);
			if (XO10(i) == 183)
				g[RA(i)] = ea;
			break;
		}
		case 215: case 247: {                                   /* stbx, stbux */
			uint32_t ea = (XO10(i) == 247 ? g[RA(i)] : RA0(i)) + g[RB(i)];
			CHECK_W(ea, 1);
			mem_w8(m, ea, (uint8_t)g[RS(i)]);
			if (XO10(i) == 247)
				g[RA(i)] = ea;
			break;
		}
		case 407: case 439: {                                   /* sthx, sthux */
			uint32_t ea = (XO10(i) == 439 ? g[RA(i)] : RA0(i)) + g[RB(i)];
			CHECK_W(ea, 2);
			mem_w16(m, ea, (uint16_t)g[RS(i)]);
			if (XO10(i) == 439)
				g[RA(i)] = ea;
			break;
		}
		case 534: {                                             /* lwbrx */
			uint32_t ea = RA0(i) + g[RB(i)];
			CHECK_R(ea, 4);
			g[RD(i)] = __builtin_bswap32(mem_r32(m, ea));
			break;
		}
		case 790: {                                             /* lhbrx */
			uint32_t ea = RA0(i) + g[RB(i)];
			CHECK_R(ea, 2);
			g[RD(i)] = __builtin_bswap16(mem_r16(m, ea));
			break;
		}
		case 662: {                                             /* stwbrx */
			uint32_t ea = RA0(i) + g[RB(i)];
			CHECK_W(ea, 4);
			mem_w32(m, ea, __builtin_bswap32(g[RS(i)]));
			break;
		}
		case 918: {                                             /* sthbrx */
			uint32_t ea = RA0(i) + g[RB(i)];
			CHECK_W(ea, 2);
			mem_w16(m, ea, __builtin_bswap16((uint16_t)g[RS(i)]));
			break;
		}
		case 20: {                                              /* lwarx */
			uint32_t ea = RA0(i) + g[RB(i)];
			CHECK_R(ea, 4);
			g[RD(i)] = mem_r32(m, ea);
			c->resv_addr = ea;
			c->resv_valid = true;
			break;
		}
		case 150: {                                             /* stwcx. */
			if (!RC(i))
				EXIT(PPC_EXIT_ILLEGAL);
			uint32_t ea = RA0(i) + g[RB(i)];
			CHECK_W(ea, 4);
			uint32_t f = so_bit(c);
			if (c->resv_valid && c->resv_addr == ea) {
				mem_w32(m, ea, g[RS(i)]);
				f |= CR_EQ;
			}
			c->resv_valid = false;
			set_crf(c, 0, f);
			break;
		}
		case 597: case 533: {                                   /* lswi, lswx */
			uint32_t ea, n;
			if (XO10(i) == 597) {
				ea = RA0(i);
				n = RB(i) ? RB(i) : 32;
			} else {
				ea = RA0(i) + g[RB(i)];
				n = c->xer & 0x7F;
			}
			if (n == 0)
				break;
			CHECK_R(ea, n);
			unsigned r = RD(i), sh = 24;
			g[r] = 0;
			for (uint32_t k = 0; k < n; k++) {
				g[r] |= (uint32_t)mem_r8(m, ea + k) << sh;
				if (sh == 0 && k + 1 < n) {
					r = (r + 1) & 31;
					g[r] = 0;
					sh = 24;
				} else {
					sh -= 8;
				}
			}
			break;
		}
		case 725: case 661: {                                   /* stswi, stswx */
			uint32_t ea, n;
			if (XO10(i) == 725) {
				ea = RA0(i);
				n = RB(i) ? RB(i) : 32;
			} else {
				ea = RA0(i) + g[RB(i)];
				n = c->xer & 0x7F;
			}
			if (n == 0)
				break;
			CHECK_W(ea, n);
			unsigned r = RS(i), sh = 24;
			for (uint32_t k = 0; k < n; k++) {
				mem_w8(m, ea + k, (uint8_t)(g[r] >> sh));
				if (sh == 0) {
					r = (r + 1) & 31;
					sh = 24;
				} else {
					sh -= 8;
				}
			}
			break;
		}
		case 1014: {                                            /* dcbz */
			uint32_t ea = (RA0(i) + g[RB(i)]) & ~31u;
			CHECK_W(ea, 32);
			memset(mem_ptr(m, ea), 0, 32);
			break;
		}
		case 54: case 86: case 246: case 278: case 758: case 982: case 598: case 854:
			/* dcbst dcbf dcbtst dcbt dcba icbi sync eieio */
			break;
		case 535: case 567: case 599: case 631: {       /* lfsx lfsux lfdx lfdux */
			bool upd = XO10(i) == 567 || XO10(i) == 631, dbl = XO10(i) >= 599;
			uint32_t ea = (upd ? g[RA(i)] : RA0(i)) + g[RB(i)];
			CHECK_R(ea, dbl ? 8 : 4);
			c->fpr[RD(i)] = dbl ? mem_r64(m, ea) : ppc_fp_single_to_double(mem_r32(m, ea));
			if (upd)
				g[RA(i)] = ea;
			break;
		}
		case 663: case 695: case 727: case 759: {       /* stfsx stfsux stfdx stfdux */
			bool upd = XO10(i) == 695 || XO10(i) == 759, dbl = XO10(i) >= 727;
			uint32_t ea = (upd ? g[RA(i)] : RA0(i)) + g[RB(i)];
			CHECK_W(ea, dbl ? 8 : 4);
			if (dbl)
				mem_w64(m, ea, c->fpr[RS(i)]);
			else
				mem_w32(m, ea, ppc_fp_double_to_single(c->fpr[RS(i)]));
			if (upd)
				g[RA(i)] = ea;
			break;
		}
		case 983: {                                     /* stfiwx */
			uint32_t ea = RA0(i) + g[RB(i)];
			CHECK_W(ea, 4);
			mem_w32(m, ea, (uint32_t)c->fpr[RS(i)]);
			break;
		}
		default:
			EXIT(PPC_EXIT_ILLEGAL);
		}
		break;

	/* D-form loads and stores */
	case 32: case 33: {                                             /* lwz, lwzu */
		uint32_t ea = (OPCD(i) == 33 ? g[RA(i)] : RA0(i)) + SIMM(i);
		CHECK_R(ea, 4);
		g[RD(i)] = mem_r32(m, ea);
		if (OPCD(i) == 33)
			g[RA(i)] = ea;
		break;
	}
	case 34: case 35: {                                             /* lbz, lbzu */
		uint32_t ea = (OPCD(i) == 35 ? g[RA(i)] : RA0(i)) + SIMM(i);
		CHECK_R(ea, 1);
		g[RD(i)] = mem_r8(m, ea);
		if (OPCD(i) == 35)
			g[RA(i)] = ea;
		break;
	}
	case 40: case 41: {                                             /* lhz, lhzu */
		uint32_t ea = (OPCD(i) == 41 ? g[RA(i)] : RA0(i)) + SIMM(i);
		CHECK_R(ea, 2);
		g[RD(i)] = mem_r16(m, ea);
		if (OPCD(i) == 41)
			g[RA(i)] = ea;
		break;
	}
	case 42: case 43: {                                             /* lha, lhau */
		uint32_t ea = (OPCD(i) == 43 ? g[RA(i)] : RA0(i)) + SIMM(i);
		CHECK_R(ea, 2);
		g[RD(i)] = (uint32_t)(int32_t)(int16_t)mem_r16(m, ea);
		if (OPCD(i) == 43)
			g[RA(i)] = ea;
		break;
	}
	case 36: case 37: {                                             /* stw, stwu */
		uint32_t ea = (OPCD(i) == 37 ? g[RA(i)] : RA0(i)) + SIMM(i);
		CHECK_W(ea, 4);
		mem_w32(m, ea, g[RS(i)]);
		if (OPCD(i) == 37)
			g[RA(i)] = ea;
		break;
	}
	case 38: case 39: {                                             /* stb, stbu */
		uint32_t ea = (OPCD(i) == 39 ? g[RA(i)] : RA0(i)) + SIMM(i);
		CHECK_W(ea, 1);
		mem_w8(m, ea, (uint8_t)g[RS(i)]);
		if (OPCD(i) == 39)
			g[RA(i)] = ea;
		break;
	}
	case 44: case 45: {                                             /* sth, sthu */
		uint32_t ea = (OPCD(i) == 45 ? g[RA(i)] : RA0(i)) + SIMM(i);
		CHECK_W(ea, 2);
		mem_w16(m, ea, (uint16_t)g[RS(i)]);
		if (OPCD(i) == 45)
			g[RA(i)] = ea;
		break;
	}
	case 46: {                                                      /* lmw */
		uint32_t ea = RA0(i) + SIMM(i);
		CHECK_R(ea, 4 * (32 - RD(i)));
		for (unsigned r = RD(i); r < 32; r++, ea += 4)
			g[r] = mem_r32(m, ea);
		break;
	}
	case 47: {                                                      /* stmw */
		uint32_t ea = RA0(i) + SIMM(i);
		CHECK_W(ea, 4 * (32 - RS(i)));
		for (unsigned r = RS(i); r < 32; r++, ea += 4)
			mem_w32(m, ea, g[r]);
		break;
	}
	case 48: case 49: case 50: case 51: {                           /* lfs lfsu lfd lfdu */
		bool upd = OPCD(i) & 1, dbl = OPCD(i) >= 50;
		uint32_t ea = (upd ? g[RA(i)] : RA0(i)) + SIMM(i);
		CHECK_R(ea, dbl ? 8 : 4);
		c->fpr[RD(i)] = dbl ? mem_r64(m, ea) : ppc_fp_single_to_double(mem_r32(m, ea));
		if (upd)
			g[RA(i)] = ea;
		break;
	}
	case 52: case 53: case 54: case 55: {                           /* stfs stfsu stfd stfdu */
		bool upd = OPCD(i) & 1, dbl = OPCD(i) >= 54;
		uint32_t ea = (upd ? g[RA(i)] : RA0(i)) + SIMM(i);
		CHECK_W(ea, dbl ? 8 : 4);
		if (dbl)
			mem_w64(m, ea, c->fpr[RS(i)]);
		else
			mem_w32(m, ea, ppc_fp_double_to_single(c->fpr[RS(i)]));
		if (upd)
			g[RA(i)] = ea;
		break;
	}
	case 59: case 63:                                               /* floating point */
		if (!ppc_fp_exec(c, i))
			EXIT(PPC_EXIT_ILLEGAL);
		break;
	case 4:                                                         /* AltiVec */
		EXIT(PPC_EXIT_UNIMPL);
	default:
		EXIT(PPC_EXIT_ILLEGAL);
	}

#undef RA0
#undef DSI
#undef CHECK_R
#undef CHECK_W
