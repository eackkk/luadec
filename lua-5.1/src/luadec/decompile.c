/* luadec, based on luac */
#include "common.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <assert.h>

#include "lua.h"
#include "lauxlib.h"
#include "ldebug.h"
#include "lobject.h"
#include "lopcodes.h"
#include "lundump.h"
#include "lstring.h"

#include "StringBuffer.h"
#include "structs.h"
#include "proto.h"
#include "ast.h"
#include "decompile.h"

#define DEBUG_PRINT

#define stddebug stdout

extern int locals;
extern int localdeclare[255][255];
extern int functionnum;
extern int process_sub;           /* process sub functions? */
extern int func_check;            /* compile decompiled function and compare */
extern int guess_locals;
extern lua_State* glstate;
extern Proto* glproto;

const char* nilstr = "nil";
char unknown_local[] = { "ERROR_unknown_local_Rxxxx" };
char unknown_upvalue[] = { "ERROR_unknown_upvalue_Rxxxx" };
StringBuffer* errorStr;
char errortmp[256];

/*
* -------------------------------------------------------------------------
*/

const char* getUpvalName(const Proto* f, int r) {
	if (f->upvalues && r < f->sizeupvalues) {
		return (char*)getstr(f->upvalues[r]);
	} else {
		sprintf(unknown_upvalue, "ERROR_unknown_upvalue_R%d", r);
		return unknown_upvalue;
	}
}

const char* getLocalName(const Proto* f, int r) {
	if (f->locvars && r < f->sizelocvars) {
		return (char*)getstr(f->locvars[r].varname);
	} else {
		sprintf(unknown_local, "ERROR_unknown_local_R%d", r);
		return unknown_local;
	}
}

char* luadec_strdup(const char* src) {
	return ((src) ? strdup(src) : NULL);
}

#define UPVALUE(r) (getUpvalName(F->f,r))
#define REGISTER(r) F->R[r]
#define PRIORITY(r) (r>=MAXSTACK ? 0 : F->Rprio[r])
#define LOCAL(r) (getLocalName(F->f,r))
#define LOCAL_STARTPC(r) F->f->locvars[r].startpc
#define PENDING(r) F->Rpend[r]
#define CALL(r) F->Rcall[r]
#define IS_TABLE(r) F->Rtabl[r]
#define IS_VARIABLE(r) F->Rvar[r]

#define fb2int(x) (luaO_fb2int(x))
#define int2fb(x) (luaO_int2fb(x))
#define MAX(a,b) (((a)>(b))?(a):(b))
#define MIN(a,b) (((a)<(b))?(a):(b))

#define SET_ERROR(F,e) { StringBuffer_printf(errorStr,"-- DECOMPILER ERROR at PC%d: %s\n", (F)->pc, (e)); RawAddStatement((F),errorStr); }
/*  error = e; errorCode = __LINE__; */ /*if (debug) { printf("DECOMPILER ERROR: %s\n", e);  }*/

extern int debug;

static char* error;
static int errorCode;

int GetJmpAddr(Function* F, int addr) {
	int real_end = addr;
	if (real_end >= F->f->sizecode) {
		real_end = F->f->sizecode;
		return real_end;
	}
	if (real_end < 0) {
		real_end = -1;
		return real_end;
	}
	while (GET_OPCODE(F->f->code[real_end]) == OP_JMP) {
		real_end = GETARG_sBx(F->f->code[real_end]) + real_end + 1;
	}
	return real_end;
}

void RawAddStatement(Function* F, StringBuffer* str);
void DeclareLocal(Function* F, int ixx, const char* value);

LoopItem* NewLoopItem(LoopType type, int prep, int start, int body, int end, int out){
	LoopItem* self = (LoopItem*)calloc(1, sizeof(LoopItem));

	self->parent = NULL;
	self->child = NULL;
	self->prev = NULL;
	self->next = NULL;

	self->type = type;
	self->prep = prep;
	self->start = start;
	self->body = body;
	self->end = end;
	self->out = out;

	return self;
}

int MatchLoopItem(LoopItem* item, LoopItem* match){
	return ((item->type == match->type)||(match->type == INT_MIN))
		&& ((item->prep == match->prep)||(match->prep == INT_MIN))
		&& ((item->start == match->start)||(match->start == INT_MIN))
		&& ((item->body == match->body)||(match->body == INT_MIN))
		&& ((item->end == match->end)||(match->end == INT_MIN))
		&& ((item->out == match->out)||(match->out == INT_MIN));
}

int AddToLoopTree(Function* F, LoopItem* item){
	while (F->loop_ptr){
		if ( item->start >= F->loop_ptr->start && item->end < F->loop_ptr->end){
			//find parent , then insert as the first child
			item->parent = F->loop_ptr;
			item->next = F->loop_ptr->child;
			item->prev = NULL;
			item->child = NULL;
			item->indent = F->loop_ptr->indent + 1;

			if ( F->loop_ptr->child){
				F->loop_ptr->child->prev = item;
			}
			F->loop_ptr->child = item;
			F->loop_ptr = item;
			return 1;
		}else{
			F->loop_ptr = F->loop_ptr->parent;
		}
	}
	return 0;
}

void DeleteLoopTree(LoopItem* item){
	LoopItem* next = item;
	while (item){
		if (item->child){
			next = item->child;
			item->child = NULL;
		}else{
			if (item->next){
				next = item->next;
				item->next = NULL;
			}else{
				next = item->parent;
			}
			free(item);
		}
		item = next;
	}
}

void DeleteLoopTree2(LoopItem* item){
	if (item == NULL) return;
	DeleteLoopTree2(item->child);
	DeleteLoopTree2(item->next);
	free(item);
}

IntListItem* NewIntListItem(int v){
	IntListItem* self = (IntListItem*)calloc(1, sizeof(IntListItem));
	self->super.prev = NULL;
	self->super.next = NULL;
	self->value = v;
	return self;
}

int MatchIntListItem(IntListItem* item, int* match_value){
	return (item->value == *match_value);
}

void DeleteIntListItem(IntListItem* item, void* dummy){
	free(item);
}

LogicExp* MakeExpNode(BoolOp* boolOp) {
	LogicExp* node = (LogicExp*)calloc(1, sizeof(LogicExp));
	node->parent = NULL;
	node->subexp = NULL;
	node->next = NULL;
	node->prev = NULL;
	node->op1 = luadec_strdup(boolOp->op1);
	node->op2 = luadec_strdup(boolOp->op2);
	node->op = boolOp->op;
	node->dest = boolOp->dest;
	node->neg = boolOp->neg;
	node->is_chain = 0;
	return node;
}

LogicExp* MakeExpChain(int dest) {
	LogicExp* node = (LogicExp*)calloc(1, sizeof(LogicExp));
	node->parent = NULL;
	node->subexp = NULL;
	node->next = NULL;
	node->prev = NULL;
	node->op1 = NULL;
	node->op2 = NULL;
	node->neg = 0;
	node->dest = dest;
	node->is_chain = 1;
	return node;
}

LogicExp* FindLogicExpTreeRoot(LogicExp* exp){
	LogicExp* curr = exp;
	while (curr->parent) {
		curr = curr->parent;
	}
	return curr;
}

void DeleteLogicExpSubTree(LogicExp* exp) {
	if (exp) {
		DeleteLogicExpSubTree(exp->subexp);
		DeleteLogicExpSubTree(exp->next);
		if(exp->op1)
			free(exp->op1);
		if(exp->op2)
			free(exp->op2);
		free(exp);
	}
}

void DeleteLogicExpTree(LogicExp* exp) {
	if (exp) {
		LogicExp* root = FindLogicExpTreeRoot(exp);
		DeleteLogicExpSubTree(root);
	}
}

void PrintLogicItem(StringBuffer* str, LogicExp* exp, int inv, int rev) {
	if (exp->subexp) {
		StringBuffer_addChar(str, '(');
		PrintLogicExp(str, exp->dest, exp->subexp, inv, rev);
		StringBuffer_addChar(str, ')');
	} else {
		char* op;
		int cond = exp->neg;
		if (inv) cond = !cond;
		if (rev) cond = !cond;
		if (cond) {
			op = invopstr(exp->op);
		}else{
			op = opstr(exp->op);
		}
		if ((exp->op != OP_TEST) && (exp->op != OP_TESTSET)) {
			StringBuffer_addPrintf(str, "%s %s %s", exp->op1, op, exp->op2);
		} else if (op) {
			StringBuffer_addPrintf(str, "%s %s", op, exp->op2);
		}else {
			StringBuffer_addPrintf(str, "%s", exp->op2);
		}
	}
}

void PrintLogicExp(StringBuffer* str, int dest, LogicExp* exp, int inv_, int rev) {
	int inv = inv_;
	while (exp->next) {
		char* op;
		int cond = exp->dest > dest;
		inv = cond ? inv_ : !inv_;
		PrintLogicItem(str, exp, inv, rev);
		exp = exp->next;
		if (inv_) cond = !cond;
		if (rev) cond = !cond;
		op = cond ? "and" : "or";
		StringBuffer_addPrintf(str, " %s ", op);
	}
	PrintLogicItem(str, exp, inv_, rev);
}

void TieAsNext(LogicExp* curr, LogicExp* item) {
	curr->next = item;
	item->prev = curr;
	item->parent = curr->parent;
}

void Untie(LogicExp* curr, int* thenaddr) {
	LogicExp* previous = curr->prev;
	if (previous) {
		previous->next = NULL;
	}
	curr->prev = NULL;
	curr->parent = NULL;
}


void TieAsSubExp(LogicExp* parent, LogicExp* item) {
	parent->subexp = item;
	while (item) {
		item->parent = parent;
		item = item->next;
	}
}

LogicExp* MakeBoolean(Function* F, int* thenaddr, int* endif) {
	int i;
	int firstaddr, elseaddr;
	BoolOp *first, *realLast, *last, *tmpLast, *curr;
	int lastCount;
	LogicExp *currExp=NULL, *firstExp=NULL;
	int dest;

	if (endif) {
		*endif = 0;
	}

	if (F->bools.size == 0) {
		SET_ERROR(F,"Attempted to build a boolean expression without a pending context");
		return NULL;
	}

	first = (BoolOp*)FirstItem(&(F->bools));
	realLast = (BoolOp*)LastItem(&(F->bools));
	last = realLast;
	firstaddr = first->pc + 2;
	*thenaddr = last->pc + 2;
	elseaddr = last->dest;

	for (curr = realLast; curr; curr = cast(BoolOp*, curr->super.prev)) {
		int dest = curr->dest;
		if ((elseaddr > *thenaddr) &&
			( ((curr->op == OP_TEST) || (curr->op == OP_TESTSET)) ? (dest > elseaddr+1) :
			(dest > elseaddr))) {
				last = curr;
				*thenaddr = curr->pc + 2;
				elseaddr = dest;
		}
	}

	tmpLast = last;
	for (curr = first; curr && curr != tmpLast; curr = cast(BoolOp*, curr->super.next)) {
		int dest = curr->dest;
		if (elseaddr > firstaddr) {
			if (dest < firstaddr) {
				last = curr;
				*thenaddr = curr->pc + 2;
				elseaddr = dest;
			}
		} else {
			if (dest == firstaddr) {
				last = curr;
				*thenaddr = curr->pc + 2;
				elseaddr = dest;
			} else {
				break;
			}
		}
	}

	dest = first->dest;
	currExp = MakeExpNode(first);

	if (dest > firstaddr && dest <= *thenaddr) {
		firstExp = MakeExpChain(dest);
		TieAsSubExp(firstExp, currExp);
	} else {
		firstExp = currExp;
		if (endif) {
			*endif = dest;
		}
	}

	if (debug) {
		printf("\n");
		for (curr = first, i = 0; curr && curr != cast(BoolOp*, last->super.next); i++, curr = cast(BoolOp*, curr->super.next)) {
			BoolOp* op = curr;
			if (debug) {
				printf("Exps(%d): at %d\tdest %d\tneg %d\t(%s %s %s) cpd %d \n", i,
					op->pc, op->dest, op->neg, op->op1, opstr(op->op), op->op2, currExp->parent ? currExp->parent->dest : -1);
			}
		}
		printf("\n");
	}

	for (curr = cast(BoolOp*, first->super.next), lastCount = 1; curr && curr != cast(BoolOp*, last->super.next); curr = cast(BoolOp*, curr->super.next), lastCount++) {
		BoolOp* op = curr;
		int at = op->pc;
		int dest = op->dest;

		LogicExp* exp = MakeExpNode(op);
		if (dest < firstaddr) {
			/* jump to loop in a while */
			TieAsNext(currExp, exp);
			currExp = exp;
			if (endif)
				*endif = dest;
		} else if (dest > *thenaddr) {
			/* jump to "else" */
			TieAsNext(currExp, exp);
			currExp = exp;
			if (endif) {
				if ((op->op != OP_TEST) && (op->op != OP_TESTSET)) {
					if (*endif != 0 && *endif != dest) {
						SET_ERROR(F,"Unhandled construct in 'MakeBoolean' P1");
						//return NULL;
					}
				}
				*endif = dest;
			}
		} else if (dest == currExp->dest) {
			/* within current chain */
			TieAsNext(currExp, exp);
			currExp = exp;
		} else if (dest > currExp->dest) {
			if (currExp->parent == NULL || dest < currExp->parent->dest) {
				/* creating a new level */
				LogicExp* subexp = MakeExpChain(dest);
				LogicExp* savecurr;
				TieAsNext(currExp, exp);
				currExp = exp;
				savecurr = currExp;
				if (currExp->parent == NULL) {
					TieAsSubExp(subexp, firstExp);
					firstExp = subexp;
				}
			} else if (dest > currExp->parent->dest) {
				/* start a new chain */
				LogicExp* prevParent;
				LogicExp* chain;
				TieAsNext(currExp, exp);
				currExp = currExp->parent;
				if (!currExp->is_chain) {
					DeleteLogicExpTree(firstExp);
					SET_ERROR(F,"Unhandled construct in 'MakeBoolean' P2");
					return NULL;
				};
				prevParent = currExp->parent;
				chain = MakeExpChain(dest);
				Untie(currExp, thenaddr);
				if (prevParent)
					if (prevParent->is_chain)
						prevParent = prevParent->subexp;
				TieAsSubExp(chain, currExp);

				currExp->parent = prevParent; // WHY comment line 483 to avoid a memory leak, but may cause logic expression output error
				if (prevParent == NULL) {
					firstExp = chain;
				} else {
					// todo
					TieAsNext(prevParent, chain);
				}
			} else {
				SET_ERROR(F,"Unhandled construct in 'MakeBoolean' P3");
				DeleteLogicExpSubTree(exp);
			}
		} else if (dest > firstaddr && dest < currExp->dest) {
			/* start a new chain */
			LogicExp* subexp = MakeExpChain(dest);
			TieAsSubExp(subexp, exp);
			TieAsNext(currExp, subexp);
			currExp = exp;
		} else {
			DeleteLogicExpSubTree(exp);
			DeleteLogicExpTree(firstExp);
			SET_ERROR(F,"Unhandled construct in 'MakeBoolean' P4");
			return NULL;
		}

		if (currExp->parent && at+3 > currExp->parent->dest) {
			currExp->parent->dest = currExp->dest;
			if (curr != last) {
				LogicExp* chain = MakeExpChain(currExp->dest);
				TieAsSubExp(chain, firstExp);
				firstExp = chain;
			}
			currExp = currExp->parent;
		}
	}
	if (firstExp->is_chain){
		firstExp = firstExp->subexp;
	}
	if (last) {
		if (F->bools.tail == (ListItem*)last) {
			F->bools.head = NULL;
			F->bools.tail = NULL;
			F->bools.size = 0;
		} else {
			F->bools.head = last->super.next;
			F->bools.head->prev = NULL;
			F->bools.size -= lastCount;
		}

	}
	curr = last; 
	while (curr) {
		BoolOp* prev = cast(BoolOp*, curr->super.prev);
		DeleteBoolOp(curr);
		curr = prev;
	}
	if (endif && *endif == 0) {
		*endif = *thenaddr;
	}
	return firstExp;
}

char* WriteBoolean(LogicExp* exp, int* thenaddr, int* endif, int test) {
	char* result = NULL;
	StringBuffer* str = StringBuffer_new(NULL);

	if (exp) {
		PrintLogicExp(str, *thenaddr, exp, 0, test);
		if (test && endif && *endif == 0) {
			//SET_ERROR(F, "Unhandled construct in 'WriteBoolean'");
			result = (char*)calloc(30, sizeof(char));
			sprintf(result,"__UNHANDLEDCONTRUCT_1__");
			goto WriteBoolean_CLEAR_HANDLER1;
		}
	} else {
		result = (char*)calloc(30, sizeof(char));
		sprintf(result, "__UNHANDLEDCONTRUCT_2__");
		goto WriteBoolean_CLEAR_HANDLER1;
	}
	result = StringBuffer_getBuffer(str);

WriteBoolean_CLEAR_HANDLER1:
	StringBuffer_delete(str);

	return result;
}

char* OutputBoolean(Function* F, int* endif, int test) {
	int thenaddr;
	char* result = NULL;
	LogicExp* exp = NULL;

	if (error) goto OutputBoolean_CLEAR_HANDLER1;
	exp = MakeBoolean(F, &thenaddr, endif);
	if (error) goto OutputBoolean_CLEAR_HANDLER1;
	result = WriteBoolean(exp, &thenaddr, endif, test);	
	if (error) goto OutputBoolean_CLEAR_HANDLER1;

OutputBoolean_CLEAR_HANDLER1:
	if (exp) DeleteLogicExpTree(exp);
	//if (error) { free(result); return NULL; }

	return result;
}

void RawAddAstStatement(Function* F, AstStatement* stmt) {
	if (F->released_local) {
		AstStatement* block = F->currStmt;
		AstStatement* curr = cast(AstStatement*, block->sub->head);
		AstStatement* tail = cast(AstStatement*, block->sub->tail);
		AstStatement* prev = NULL;
		int count = 0;
		int lpc = F->released_local;
		F->released_local = 0;
		while (curr) {
			if (curr->line >= lpc) {
				break;
			}
			prev = curr;
			curr = cast(AstStatement*, prev->super.next);
			count++;
		}
		if (curr) {
			//TODO check list.size
			int blockSize = block->sub->size;

			AstStatement* dostmt = MakeBlockStatement(BLOCK_STMT, NULL);
			dostmt->line = lpc;

			while (curr) {
				AstStatement* next = cast(AstStatement*, curr->super.next);
				RemoveFromList(block->sub, cast(ListItem*,curr));
				AddToStatement(dostmt, curr);
				curr = next;
			}
			AddToStatement(block, dostmt);
			F->currStmt = dostmt;
		}
	}
	stmt->line = F->pc;
	AddToStatement(F->currStmt, stmt);
	F->lastLine = F->pc;
}

void RawAddStatement(Function* F, StringBuffer* str) {
	AstStatement* stmt = MakeSimpleStatement(StringBuffer_getBuffer(str));
	stmt->line = F->pc;
	RawAddAstStatement(F, stmt);
}

void FlushWhile1(Function* F) {
	LoopItem* walk = F->loop_ptr;
	StringBuffer* str = StringBuffer_new(NULL);

	if (walk->type == WHILE && walk->start <= F->pc && walk->body == -1) {
		AstStatement* whilestmt = MakeBlockStatement(WHILE_STMT, strdup("1"));
		RawAddAstStatement(F, whilestmt);
		F->currStmt = whilestmt;
		walk->body = walk->start;
		walk = walk->parent;
	}

	StringBuffer_delete(str);
}

void FlushBoolean(Function* F) {
	if (F->bools.size == 0) {
		FlushWhile1(F);
	}
	while (F->bools.size > 0) {
		int flushWhile = 0;
		int endif = 0, thenaddr = 0;
		char* test = NULL;
		StringBuffer* str = StringBuffer_new(NULL);
		LogicExp* exp = NULL;
		LoopItem* walk = NULL;

		exp = MakeBoolean(F, &thenaddr, &endif);
		if (error) goto FlushBoolean_CLEAR_HANDLER1;

		test = WriteBoolean(exp, &thenaddr, &endif, 0);
		if (error) goto FlushBoolean_CLEAR_HANDLER1;

		//TODO find another method to determine while loop body to output while do
		//search parent
		walk = F->loop_ptr;
		if (walk->type == WHILE && walk->out == endif -1 && walk->body == -1){
			int whileStart = walk->start;
			walk->body = thenaddr;
			flushWhile = 1;
		}

		if (flushWhile){
			AstStatement* whilestmt = MakeBlockStatement(WHILE_STMT, test);
			test = NULL;
			RawAddAstStatement(F, whilestmt);
			F->currStmt = whilestmt;
		} else {
			AstStatement* ifstmt = NULL;
			FlushWhile1(F);
			ifstmt = MakeIfStatement(test);
			ThenStart(ifstmt) = thenaddr;
			ElseStart(ifstmt) = endif - 1;
			test = NULL;
			RawAddAstStatement(F, ifstmt);
			F->currStmt = ThenStmt(ifstmt);
		}

FlushBoolean_CLEAR_HANDLER1:
		if (exp) DeleteLogicExpTree(exp);
		if (test) free(test);
		StringBuffer_delete(str);
		if (error) return;
	}
	F->testpending = 0;
}

void DeclareLocalsAddStatement(Function* F, StringBuffer* statement) {
	if (F->pc > 0) {
		FlushBoolean(F);
		if (error) return;
	}
	RawAddStatement(F, statement);
}

void AddStatement(Function* F, StringBuffer* statement) {
	FlushBoolean(F);
	if (error) return;

	RawAddStatement(F, statement);
}

void AddAstStatement(Function* F, AstStatement* stmt) {
	FlushBoolean(F);
	if (error) return;

	RawAddAstStatement(F, stmt);
}

/*
* -------------------------------------------------------------------------
*/

void DeclarePendingLocals(Function* F);

void AssignGlobalOrUpvalue(Function* F, const char* dest, const char* src) {
	F->testjump = 0;
	AddToVarList(&(F->vpend), luadec_strdup(dest), luadec_strdup(src), -1);
}

void AssignReg(Function* F, int reg, const char* src, int prio, int mayTest) {
	char* dest = REGISTER(reg);
	char* nsrc = NULL;

	if (PENDING(reg)) {
		if (guess_locals) {
			sprintf(errortmp, "Overwrote pending register: R%d in 'AssignReg'", reg);
			SET_ERROR(F, errortmp);
		} else {
			sprintf(errortmp, "Overwrote pending register: R%d in 'AssignReg'. Creating missing local", reg);
			SET_ERROR(F, errortmp);
			DeclareLocal(F, reg, dest);
		}
		return;
	}

	PENDING(reg) = 1;
	CALL(reg) = 0;
	F->Rprio[reg] = prio;

	if (debug) {
		printf("SET_SIZE(tpend) = %d \n", SET_SIZE(F->tpend));
	}

	nsrc = luadec_strdup(src);
	if (F->testpending == reg+1 && mayTest && F->testjump == F->pc+2) {
		int endif;
		char* test = OutputBoolean(F, &endif, 1);
		if (error) {
			free(nsrc);
			if (test) free(test);
			return;
		}
		if (endif >= F->pc) {
			StringBuffer* str = StringBuffer_new(NULL);
			StringBuffer_printf(str, "%s or %s", test, src);
			free(nsrc);
			nsrc = StringBuffer_getBuffer(str);
			StringBuffer_delete(str);
			F->testpending = 0;
			F->Rprio[reg] = 8;
		}
		if (test) free(test);
	}
	F->testjump = 0;

	if (!IS_VARIABLE(reg)) {
		if (REGISTER(reg)) free(REGISTER(reg));
		REGISTER(reg) = nsrc;
		AddToSet(F->tpend, reg);
	} else {
		AddToVarList(&(F->vpend), luadec_strdup(dest), nsrc, reg);
	}
}

/*
** Table Functions
*/

DecTableItem* NewTableItem(const char* value, int num, const char* key) {
	DecTableItem* self = (DecTableItem*)calloc(1, sizeof(DecTableItem));
	self->super.prev = NULL;
	self->super.next = NULL;
	self->key = luadec_strdup(key);
	self->value = luadec_strdup(value);
	self->numeric = num;
	return self;
}

void ClearTableItem(DecTableItem* item, void* dummy) {
	if (item) {
		if (item->key) {
			free(item->key);
		}
		if (item->value) {
			free(item->value);
		}
	}
}

void DeleteTableItem(DecTableItem* item) {
	ClearTableItem(item, NULL);
	free(item);
}

int MatchTable(DecTable* tbl, int* reg) {
	return tbl->reg == *reg;
}

void DeleteTable(DecTable* tbl) {
	ClearList(&(tbl->keyed), (ListItemFn)ClearTableItem);
	ClearList(&(tbl->numeric), (ListItemFn)ClearTableItem);
	free(tbl);
}

void CloseTable(Function* F, int r) {
	DecTable* tbl = (DecTable*)RemoveFromList(&(F->tables), FindFromListTail(&(F->tables), (ListItemCmpFn)MatchTable, &r));
	DeleteTable(tbl);
	F->Rtabl[r] = 0;
}

void PrintTableItemNumeric(StringBuffer* str, DecTableItem* item) {
	char* value = item->value;
	if (value[0] == '{' && StringBuffer_lastChar(str) != '\n') {
		StringBuffer_add(str, "\n");
	}
	StringBuffer_add(str, value);
	if (value[strlen(value) - 1] == '}') {
		StringBuffer_add(str, "\n");
	}
}

void PrintTableItemKeyed(Function* F, StringBuffer* str, DecTableItem* item) {
	char* value = item->value;
	if (value[0] == '{' && StringBuffer_lastChar(str) != '\n') {
		StringBuffer_add(str, "\n");
	}
	MakeIndex(F, str, item->key, TABLE);
	StringBuffer_addPrintf(str, " = %s", item->value);
	if (value[strlen(value) - 1] == '}') {
		StringBuffer_add(str, "\n");
	}
}

char* PrintTable(Function* F, int r, int returnCopy) {
	char* result = NULL;
	int numerics = 0;
	DecTableItem* item;
	StringBuffer* str = StringBuffer_new("{");
	DecTable* tbl = (DecTable*)FindFromListTail(&(F->tables), (ListItemCmpFn)MatchTable, &r);
	if (tbl == NULL) {
		F->Rtabl[r] = 0;
		return F->R[r];
	}
	item = cast(DecTableItem*, tbl->numeric.head);
	if (item) {
		numerics = 1;
		PrintTableItemNumeric(str, item);
		item = cast(DecTableItem*, item->super.next);
		while (item) {
			StringBuffer_add(str, ", ");
			PrintTableItemNumeric(str, item);
			item = cast(DecTableItem*, item->super.next);
		}
	}
	item = cast(DecTableItem*, tbl->keyed.head);
	if (item) {
		if (numerics) {
			StringBuffer_add(str, "; ");
		}
		PrintTableItemKeyed(F, str, item);
		item = cast(DecTableItem*, item->super.next);
		while (item) {
			StringBuffer_add(str, ", ");
			PrintTableItemKeyed(F, str, item);
			item = cast(DecTableItem*, item->super.next);
		}
	}
	StringBuffer_addChar(str, '}');
	PENDING(r) = 0;
	AssignReg(F, r, StringBuffer_getRef(str), 0, 0);
	if (error) {
		result = NULL;
	}else if (returnCopy){
		result = StringBuffer_getBuffer(str);
	}
	StringBuffer_delete(str);
	CloseTable(F, r);
	return result;
}

DecTable* NewTable(int r, Function* F, int b, int c, int pc) {
	DecTable* self = (DecTable*)calloc(1, sizeof(DecTable));
	self->super.prev = NULL;
	self->super.next = NULL;
	InitList(&(self->numeric));
	InitList(&(self->keyed));
	self->reg = r;
	self->topNumeric = 0;
	self->F = F;
	self->arraySize = fb2int(b);
	self->keyedSize = fb2int(c); //1<<c;
	self->pc = pc;
	PENDING(r) = 1;
	return self;
}

void AddToTable(Function* F, DecTable* tbl, const char* value, const char* key) {
	DecTableItem* item;
	List* type;
	int index;
	if (key == NULL) {
		type = &(tbl->numeric);
		index = tbl->topNumeric;
		tbl->topNumeric++;
	} else {
		type = &(tbl->keyed);
		tbl->used++;
		index = 0;
	}
	item = NewTableItem(value, index, key);
	AddToList(type, (ListItem*)item);
}

void StartTable(Function* F, int r, int b, int c, int pc) {
	DecTable* tbl = NewTable(r, F, b, c, pc);
	AddToListHead(&(F->tables), (ListItem*)tbl);
	F->Rtabl[r] = 1;
}

void SetList(Function* F, int a, int b, int c) {
	int i;
	DecTable* tbl = (DecTable*)FindFromListTail(&(F->tables), (ListItemCmpFn)MatchTable, &a);
	if (tbl == NULL) {
		sprintf(errortmp, "No list found for R%d , SetList fails", a);
		SET_ERROR(F, errortmp);
		return;
	}
	if (b == 0) {
		const char* rstr;
		i = 1;
		while (1) {
			rstr = GetR(F, a + i);
			if (error)
				return;
			if (strcmp(rstr,".end") == 0)
				break;
			AddToTable(F, tbl, rstr, NULL); // Lua5.1 specific TODO: it's not really this :(
			i++;
		};
	} //should be {...} or func(func()) ,when b == 0, that will use all avaliable reg from R(a)

	for (i = 1; i <= b; i++) {
		const char* rstr = GetR(F, a + i);
		if (error)
			return;
		AddToTable(F, tbl, rstr, NULL); // Lua5.1 specific TODO: it's not really this :(
	}
}

void UnsetPending(Function* F, int r) {
	if (!IS_VARIABLE(r)) {
		if (!PENDING(r) && !CALL(r)) {
			if (guess_locals) {
				sprintf(errortmp, "Confused about usage of register: R%d in 'UnsetPending'", r);
				SET_ERROR(F, errortmp);
			} else {
				sprintf(errortmp, "Confused about usage of register: R%d in 'UnsetPending'. Creating missing local", r);
				SET_ERROR(F, errortmp);
				DeclareLocal(F, r, REGISTER(r));
			}
			return;
		}
		PENDING(r) = 0;
		RemoveFromSet(F->tpend, r);
	}
}

int SetTable(Function* F, int a, char* bstr, char* cstr) {
	DecTable* tbl = (DecTable*)FindFromListTail(&(F->tables), (ListItemCmpFn)MatchTable, &a);
	if (tbl==NULL) {
		UnsetPending(F, a);
		return 0;
	}
	AddToTable(F, tbl, cstr, bstr);
	return 1;
}

/*
** BoolOp Functions
*/

BoolOp* NewBoolOp() {
	BoolOp* value = (BoolOp*)calloc(1, sizeof(BoolOp));
	value->super.prev = NULL;
	value->super.next = NULL;
	value->op1 = NULL;
	value->op2 = NULL;
	return value;
}

BoolOp* MakeBoolOp(char* op1, char* op2, OpCode op, int neg, int pc, int dest) {
	BoolOp* value = (BoolOp*)calloc(1, sizeof(BoolOp));
	value->super.prev = NULL;
	value->super.next = NULL;
	value->op1 = op1;
	value->op2 = op2;
	value->op = op;
	value->neg = neg;
	value->pc = pc;
	value->dest = dest;
	return value;
}

void ClearBoolOp(BoolOp* ptr, void* dummy) {
	if (ptr) {
		if (ptr->op1) {
			free(ptr->op1);
			ptr->op1 = NULL;
		}
		if (ptr->op2) {
			free(ptr->op2);
			ptr->op2 = NULL;
		}
	}
}

void DeleteBoolOp(BoolOp* ptr){
	ClearBoolOp(ptr, NULL);
	free(ptr);
}

/*
** -------------------------------------------------------------------------
*/

Function* NewFunction(const Proto* f) {
	Function* self = (Function*)calloc(1, sizeof(Function));
	self->f = f;
	InitList(&(self->vpend));
	self->tpend = NewIntSet(0);

	self->loop_tree = NewLoopItem(FUNC_ROOT,-1,-1,0,f->sizecode-1,f->sizecode);
	self->loop_ptr = self->loop_tree;

	self->funcBlock = MakeBlockStatement(FUNCTION_STMT, NULL);
	self->currStmt = self->funcBlock;

	InitList(&(self->breaks));
	InitList(&(self->continues));
	InitList(&(self->jmpdests));

	self->do_opens = NewIntSet(0);
	self->do_closes = NewIntSet(0);
	self->decompiledCode = StringBuffer_new(NULL);

	InitList(&(self->bools));

	self->intspos = 0;

	self->funcnumstr = NULL;
	return self;
}

void DeleteFunction(Function* self) {
	int i;
	DeleteAstStatement(self->funcBlock);
	ClearList(&(self->bools), (ListItemFn)ClearBoolOp);
	/*
	* clean up registers
	*/
	for (i = 0; i < MAXARG_A; i++) {
		if (self->R[i]){
			free(self->R[i]);
		}
	}
	StringBuffer_delete(self->decompiledCode);
	ClearList(&(self->vpend), (ListItemFn)ClearVarListItem);
	DeleteIntSet(self->tpend);
	ClearList(&(self->breaks), NULL);
	ClearList(&(self->continues), NULL);
	ClearList(&(self->jmpdests), (ListItemFn)ClearAstStatement);
	DeleteLoopTree(self->loop_tree);
	DeleteIntSet(self->do_opens);
	DeleteIntSet(self->do_closes);
	if (self->funcnumstr){
		free(self->funcnumstr);
	}
	free(self);
}

void DeclareVariable(Function* F, const char* name, int reg);

const char* GetR(Function* F, int r) {
	if (IS_TABLE(r)) {
		PrintTable(F, r, 0);
		if (error) return NULL;
	}
	UnsetPending(F, r);
	if (error) return NULL;

	if (F->R[r] == NULL) {
		char sb[] = { "R%rrrrr%_PC%pcccccccc%" };
		sprintf(sb, "R%d_PC%d", r, F->pc);
		DeclareVariable(F, sb, r);
	}//dirty hack , some numeric FOR loops may cause error
	return F->R[r];
}

void DeclareVariable(Function* F, const char* name, int reg) {
	F->Rvar[reg] = 1;
	if (F->R[reg]) {
		free(F->R[reg]);
	}
	F->R[reg] = luadec_strdup(name);
	F->Rprio[reg] = 0;
	UnsetPending(F, reg);
	if (error) return;
}

void OutputAssignments(Function* F) {
	int i, srcs;
	ListItem *walk, *tail;
	StringBuffer* vars = StringBuffer_new(NULL);
	StringBuffer* exps = StringBuffer_new(NULL);
	if (!SET_IS_EMPTY(F->tpend)) {
		goto OutputAssignments_ERROR_HANDLER;
	}
	srcs = 0;
	walk = F->vpend.head;
	tail = F->vpend.tail;
	i = 0;
	while (walk) {
		int r = cast(VarListItem*, walk)->reg;
		char* src = cast(VarListItem*, walk)->src;
		char* dest = cast(VarListItem*, walk)->dest;
		if (!(r == -1 || PENDING(r))) {
			sprintf(errortmp, "Confused about usage of register: R%d in 'OutputAssignments'", r);
			SET_ERROR(F, errortmp);
			goto OutputAssignments_ERROR_HANDLER;
		}

		if (i > 0) {
			StringBuffer_prepend(vars, ", ");
		}
		StringBuffer_prepend(vars, dest);

		if (src && (srcs > 0 || (srcs == 0 && strcmp(src, "nil") != 0) || walk == tail )) {
			if (srcs > 0) {
				StringBuffer_prepend(exps, ", ");
			}
			StringBuffer_prepend(exps, src);
			srcs++;
		}
		if (r != -1) {
			PENDING(r) = 0;
		}
		walk = walk->next;
		i++;
	}

	if (i > 0) {
		StringBuffer_add(vars, " = ");
		StringBuffer_add(vars, StringBuffer_getRef(exps));
		AddStatement(F, vars);
		if (error) {
			goto OutputAssignments_ERROR_HANDLER;
		}
	}
OutputAssignments_ERROR_HANDLER:
	ClearList(&(F->vpend), (ListItemFn)ClearVarListItem);
	StringBuffer_delete(vars);
	StringBuffer_delete(exps);
}

void ReleaseLocals(Function* F) {
	int i;
	for (i = F->f->sizelocvars-1; i >=0 ; i--) {
		if (F->f->locvars[i].endpc == F->pc) {
			int r;
			F->freeLocal--;
			if (F->freeLocal < 0) {
				F->freeLocal = 0;
				fprintf(stderr, "freeLocal<0 in void ReleaseLocals(Function* F)\n");
				fprintf(stderr, " at line %d in file %s\n", __LINE__, __FILE__);
				fprintf(stderr, " for lua files: ");
				printFileNames(stderr);
				fprintf(stderr, "\n");
				fprintf(stderr, " at lua function %s pc=%d\n\n", F->funcnumstr, F->pc);
				fflush(stderr);
				SET_ERROR(F, "freeLocal<0 in 'ReleaseLocals'");
				return;
			}
			r = F->freeLocal;
			//fprintf(stderr,"%d %d %d\n",i,r, F->pc);
			if (!IS_VARIABLE(r)) {
				// fprintf(stderr,"--- %d %d\n",i,r);
				sprintf(errortmp, "Confused about usage of register R%d for local variables in 'ReleaseLocals'", r);
				SET_ERROR(F, errortmp);
				return;
			}
			F->Rvar[r] = 0;
			F->Rprio[r] = 0;
			if (!F->ignore_for_variables && !F->released_local) {
				F->released_local = F->f->locvars[i].startpc;
			}
		}
	}
	F->ignore_for_variables = 0;
}

void DeclareLocals(Function* F) {
	int i;
	int locals;
	int internalLocals = 0;
	//int loopstart;
	//int loopvars;
	int loopconvert;
	StringBuffer *str, *rhs;
	char* names[MAXARG_A];
	int startparams = 0;
	/*
	* Those are declaration of parameters.
	*/
	if (F->pc == 0) {
		startparams = F->f->numparams;
		if ((F->f->is_vararg&1) && (F->f->is_vararg&2)) {
			startparams++;
		}
	}
	str = StringBuffer_new("local ");
	rhs = StringBuffer_new(" = ");
	locals = 0;
	if (F->pc != 0) {
		for (i=startparams; i < F->f->maxstacksize; i++) {
			if (functionnum >=0 && functionnum < 255 && localdeclare[functionnum][i]==F->pc) {
				int r = i;
				char* name = (char*)calloc(12, sizeof(char));
				sprintf(name,"l_%d_%d",functionnum,i);
				if (F->Rinternal[r]) {
					names[r] = name;
					F->Rinternal[r] = 0;
					internalLocals++;
					continue;
				}
				if (PENDING(r)) {
					if (locals > 0) {
						StringBuffer_add(str, ", ");
						StringBuffer_add(rhs, ", ");
					}
					StringBuffer_add(str, name);
					StringBuffer_add(rhs, GetR(F, r));
				} else {
					StringBuffer_add(str, ", ");
					StringBuffer_add(str, name);
				}
				CALL(r) = 0;
				IS_VARIABLE(r) = 1;
				names[r] = name;
				locals++;
			}
		}
	}
	loopconvert = 0;
	for (i = startparams; i < F->f->sizelocvars; i++) {
		if (F->f->locvars[i].startpc == F->pc) {
			int r = F->freeLocal + locals + internalLocals;
			Instruction instr = F->f->code[F->pc];
			// handle FOR loops
			if (GET_OPCODE(instr) == OP_FORPREP) {
				F->f->locvars[i].startpc = F->pc+1;
				continue;
			}
			// handle TFOR loops
			if (GET_OPCODE(instr) == OP_JMP) {
				Instruction n2 = F->f->code[F->pc+1+GETARG_sBx(instr)];
				//fprintf(stderr,"3 %d\n",F->pc+1+GETARG_sBx(instr));
				//fprintf(stderr,"4 %s %d\n",luaP_opnames[GET_OPCODE(n2)], F->pc+GETARG_sBx(instr));
				if (GET_OPCODE(n2) == OP_TFORLOOP) {
					F->f->locvars[i].startpc = F->pc+1;
					continue;
				}
			}
			if ((F->Rinternal[r])) {
				names[r] = luadec_strdup(LOCAL(i));
				PENDING(r) = 0;
				IS_VARIABLE(r) = 1;
				F->Rinternal[r] = 0;
				internalLocals++;
				continue;
			}
			if (PENDING(r)) {
				if (locals > 0) {
					StringBuffer_add(str, ", ");
					StringBuffer_add(rhs, ", ");
				}
				StringBuffer_add(str, LOCAL(i));
				StringBuffer_add(rhs, GetR(F, r));
				if (error) return;
			} else {
				if (locals > 0){
					StringBuffer_add(str, ", ");
				}
				StringBuffer_add(str, LOCAL(i));
			}
			CALL(r) = 0;
			IS_VARIABLE(r) = 1;
			names[r] = luadec_strdup(LOCAL(i));
			locals++;
		}
	}
	if (locals > 0) {
		StringBuffer_add(str, StringBuffer_getRef(rhs));
		if (strcmp(StringBuffer_getRef(rhs)," = ") == 0){
			StringBuffer_add(str,"nil");
		}
		DeclareLocalsAddStatement(F, str);
		if (error) return;
	}
	StringBuffer_delete(rhs);
	StringBuffer_delete(str);
	for (i = 0; i < locals + internalLocals; i++) {
		int r = F->freeLocal + i;
		DeclareVariable(F, names[r], r);
		if (names[r]) free(names[r]);
		if (error) return;
	}
	F->freeLocal += locals + internalLocals;
}

void PrintFunctionCheck(Function* F) {
}

char* PrintFunction(Function* F) {
	StringBuffer* buff = F->decompiledCode;
	int indent = F->indent;
	PrintFunctionCheck(F);
	StringBuffer_prune(buff);

	if (IsMain(F->f)) {
		PrintAstSub(F->funcBlock, buff, 0);
	} else {
		StringBuffer_addPrintf(buff, "function(%s)\n", F->funcBlock->code);
		PrintAstSub(F->funcBlock, buff, indent+1);
		PrintIndent(buff, indent);
		StringBuffer_add(buff, "end\n");
	}

	return StringBuffer_getBuffer(F->decompiledCode);
}

/*
** -------------------------------------------------------------------------
*/

char* RegisterOrConstant(Function* F, int r) {
	if (IS_CONSTANT(r)) {
		return DecompileConstant(F->f, INDEXK(r));
	} else {
		return luadec_strdup(GetR(F, r));
	}
}

// isalpha in stdlib is undefined when ch>=256 , may throw a assertion error.
int luadec_isalpha(int ch) {
	return ( ch>='A' && ch<='Z' ) || ( ch>='a' && ch<='z' );
}
int luadec_isdigit(int ch) {
	return ( ch>='0' && ch<='9' );
}
int luadec_isalnum(int ch) {
	return ( ch>='0' && ch<='9' ) || ( ch>='a' && ch<='z' ) || ( ch>='A' && ch<='Z' );
}

const int numofkeywords = 23;
const char* keywords[] = {
	"and", "break", "do", "else", "elseif",
	"end", "false", "for", "function", "if",
	"in", "local", "nil", "not", "or",
	"repeat", "return", "then", "true", "until",
	"while", "continue", "goto"
};

int isIdentifier(const char* src) {
	int dot = 0;
	if (luadec_isalpha(src[0]) || src[0] == '_') {
		int i;
		const char *at;
		int len = strlen(src);
		dot = 1;
		for (at = src + 1; at < src + len; at++) {
			if (!(luadec_isalnum(*at) || *at == '_')) {
				dot = 0;
				break;
			}
		}
		for (i = 0; i < numofkeywords; i++){
			if (strcmp(keywords[i], src) == 0){
				dot = 0;
				break;
			}
		}
	}
	return dot;
}

/*
** type: DOT=0,SELF=1,TABLE=2
** input and output
** rstr   "a"  " a"    "or"    a       a+2
** SELF   :a   ERROR   ERROR   ERROR   ERROR
** DOT    .a   [" a"]  ["or"]  [a]     [a+2]
** TABLE   a   [" a"]  ["or"]  [a]     [a+2]
*/
void MakeIndex(Function* F, StringBuffer* str, char* rstr, IndexType type) {
	int len = strlen(rstr);
	/*
	* see if index can be expressed without quotes
	*/
	if (rstr[0] == '\"' && rstr[len-1] == '\"') {
		rstr[len - 1] = '\0';
		if (isIdentifier((rstr + 1))) {
			// type value DOT=0;SELF=1;TABLE=2;
			switch (type) {
			case SELF:
				StringBuffer_addPrintf(str, ":%s", (rstr + 1));
				break;
			case DOT:
				StringBuffer_addPrintf(str, ".%s", (rstr + 1));
				break;
			case TABLE:
				StringBuffer_addPrintf(str, "%s", (rstr + 1));
				break;
			}
			rstr[len - 1] = '\"';
			return;
		}
		rstr[len - 1] = '\"';
	}

	if (type != SELF) {
		StringBuffer_addPrintf(str, "[%s]", rstr);
		return;
	} else {
		char* errorbuff = (char*)calloc((len + 50), sizeof(char));
		StringBuffer_addPrintf(str, ":[%s]", rstr);
		sprintf(errorbuff, "[%s] should be a SELF Operator", rstr);
		SET_ERROR(F, errorbuff);
		free(errorbuff);
	}
}

void FunctionHeader(Function* F) {
	const Proto* f = F->f;
	StringBuffer* str = StringBuffer_new(NULL);
	if (f->numparams > 0) {
		int i = 0;
		StringBuffer_set(str, LOCAL(i));
		for (i = 1; i < f->numparams; i++){
			StringBuffer_addPrintf(str, ", %s", LOCAL(i));
		}
		if (f->is_vararg) {
			StringBuffer_add(str, ", ...");
		}
	} else if (f->is_vararg) {
		StringBuffer_set(str, "...");
	} else {
		StringBuffer_set(str, "");
	}
	F->funcBlock->code = StringBuffer_getBuffer(str);
	StringBuffer_delete(str);
}

void ShowState(Function* F) {
	int i;
	ListItem* walk;
	fprintf(stddebug, "\n");
	fprintf(stddebug, "next bool: %d\n", F->bools.size);
	fprintf(stddebug, "locals(%d): ", F->freeLocal);
	for (i = 0; i < F->freeLocal; i++) {
		fprintf(stddebug, "%d{%s} ", i, REGISTER(i));
	}
	fprintf(stddebug, "\n");
	fprintf(stddebug, "vpend(%d): ", F->vpend.size);

	walk = F->vpend.head;
	i = 0;
	while (walk) {		
		int r = cast(VarListItem*, walk)->reg;
		char* src = cast(VarListItem*, walk)->src;
		char* dest = cast(VarListItem*, walk)->dest;
		if (r != -1 && !PENDING(r)) {
			sprintf(errortmp, "Confused about usage of register R%d for variables", r);
			SET_ERROR(F, errortmp);
			return;
		}
		fprintf(stddebug, "%d{%s=%s} ", r, dest, src);
		walk = walk->next;
	}
	fprintf(stddebug, "\n");
	fprintf(stddebug, "tpend(%d): ", SET_SIZE(F->tpend));
	walk = F->tpend->list.head;
	while (walk) {
		int r = cast(IntSetItem*, walk)->value;
		fprintf(stddebug, "%d{%s} ", r, REGISTER(r));
		if (!PENDING(r)) {
			sprintf(errortmp, "Confused about usage of register R%d for temporaries", r);
			SET_ERROR(F, errortmp);
			return;
		}
		walk = walk->next;
	}
	fprintf(stddebug, "\n");
}

#define TRY(x)  x; if (error) goto errorHandler

void DeclareLocal(Function* F, int ixx, const char* value) {
	if (!IS_VARIABLE(ixx)) {
		char x[10];
		StringBuffer* str = StringBuffer_new(NULL);

		sprintf(x,"l_%d_%d",functionnum, ixx);
		DeclareVariable(F, x, ixx);
		IS_VARIABLE(ixx) = 1;
		StringBuffer_printf(str,"local %s = %s",x,value);
		RawAddStatement(F, str);
		F->freeLocal++;
		StringBuffer_delete(str);
	}
}

void DeclarePendingLocals(Function* F) {
	StringBuffer* str = StringBuffer_new(NULL);
	if (SET_SIZE(F->tpend)>0) {
		if (guess_locals) {
			StringBuffer_set(str,"-- WARNING: pending registers.");
		} else {
			ListItem* walk = F->tpend->list.head;
			StringBuffer_set(str,"-- WARNING: pending registers. Declaring locals.");
			AddStatement(F,str);
			while (walk) {
				int reg = cast(IntSetItem*, walk)->value;
				char* s = luadec_strdup(REGISTER(reg));
				GetR(F, reg);
				DeclareLocal(F, reg, s);
				free(s);
				walk = walk->next;
			}
		}
	}
	StringBuffer_delete(str);
}

Proto* toproto(lua_State* L, int i);

int FunctionCheck(Proto* f, const char* funcnumstr, StringBuffer* str) {
	lua_State* newState;
	int check_result;
	char* decompiled = ProcessSubFunction(f, 1, luadec_strdup(funcnumstr));
	newState = lua_open();
	if (luaL_loadstring(newState, decompiled) != 0) {
		check_result = -1;
		StringBuffer_printf(str, "-- function check fail %s : cannot compile", funcnumstr);
	} else {
		StringBuffer* compare_result_str = StringBuffer_newBySize(127);
		Proto* newProto = toproto(newState, -1);;
		if (!IsMain(f)) {
			newProto = newProto->p[0];
		}
		check_result = CompareProto(f, newProto, compare_result_str);
		if (check_result == 0) {
			StringBuffer_printf(str, "-- function check pass %s", funcnumstr);
		} else {
			StringBuffer_printf(str, "-- function check fail %s : %s", funcnumstr, StringBuffer_getRef(compare_result_str));
		}
		StringBuffer_delete(compare_result_str);
	}

	lua_close(newState);
	free(decompiled);
	return check_result;
}

int CompareProto(const Proto* forg, const Proto* fnew, StringBuffer* str) {
	int sizesame, pc, minsizecode;
	int diff = 0;
	StringBuffer_set(str, "");
	if (forg->numparams != fnew->numparams) {
		diff++;
		StringBuffer_add(str, " different params size;");
	}
	if (forg->nups != fnew->nups) {
		diff++;
		StringBuffer_add(str, " different upvalues size;");
	}
	if (forg->is_vararg != fnew->is_vararg) {
		diff++;
		StringBuffer_add(str, " different is_vararg;");
	}
	if (forg->sizecode != fnew->sizecode) {
		diff++;
		StringBuffer_add(str, " different code size;");
	}
	sizesame = 0;
	minsizecode = MIN(forg->sizecode, fnew->sizecode);
	for (pc = 0; pc < minsizecode; pc++){
		Instruction iorg = forg->code[pc];
		Instruction inew = fnew->code[pc];
		if (iorg == inew) {
			sizesame++;
		}
	}
	if (sizesame != forg->sizecode) {
		diff++;
		StringBuffer_addPrintf(str, " sizecode org: %d, decompiled: %d, same: %d;", forg->sizecode, fnew->sizecode, sizesame);
	}
	return diff;
}

char* PrintFunctionOnlyParamsAndUpvalues(const Proto* f, int indent, char* funcnumstr) {
	int i = 0;
	int baseIndent = indent;
	char* output = NULL;
	StringBuffer* str = StringBuffer_new(NULL);
	Function* F = NewFunction(f);
	F->loop_tree->indent = indent;
	F->indent = indent;
	F->funcnumstr = funcnumstr;
	error = NULL;

	/*
	* Function parameters are stored in registers from 0 on.
	*/
	for (i = 0; i < f->numparams; i++) {
		char* x = (char*)calloc(MAX(10,strlen(LOCAL(i))+1), sizeof(char));
		sprintf(x,"%s",LOCAL(i));
		//sprintf(x,"l_%d_%d",functionnum, i);
		TRY(DeclareVariable(F, x, i));
		IS_VARIABLE(i) = 1;
	}
	F->freeLocal = f->numparams;

	TRY(FunctionHeader(F));
	F->indent++;

	StringBuffer_printf(str, "-- function num : %s", funcnumstr);
	TRY(RawAddStatement(F, str));

	if (f->sizeupvalues > 0) {
		StringBuffer_set(str, "local _upvalues_ = {");
		listUpvalues(F->f, str);
		StringBuffer_add(str, "}");
		TRY(RawAddStatement(F, str));
	}

	F->indent--;

errorHandler:
	output = PrintFunction(F);
	DeleteFunction(F);
	StringBuffer_delete(str);
	return output;
}

int listUpvalues(const Proto* f, StringBuffer* str) {
	int i = 0;
	StringBuffer_add(str, getUpvalName(f, i));
	for (i = 1; i < f->sizeupvalues; i++) {
		StringBuffer_addPrintf(str, ", %s", getUpvalName(f, i));
	}
	return f->sizeupvalues;
}

int isTestOpCode(OpCode op) {
	return ( op == OP_EQ || op == OP_LE || op == OP_LT || op == OP_TEST || op == OP_TESTSET );
}

char* ProcessCode(Proto* f, int indent, int func_checking, char* funcnumstr) {
	int i = 0;

	int ignoreNext = 0;

	Function* F;
	StringBuffer* str = StringBuffer_new(NULL);

	const Instruction* code = f->code;
	int pc, n = f->sizecode;
	int baseIndent = indent;

	char* output;

	LoopItem* next_child;

	F = NewFunction(f);
	F->funcnumstr = funcnumstr;
	F->indent = indent;
	F->pc = 0;
	error = NULL;

	/*
	* Function parameters are stored in registers from 0 on.
	*/
	for (i = 0; i < f->numparams; i++) {
		char* x = (char*)calloc(MAX(10,strlen(LOCAL(i))+1), sizeof(char));
		sprintf(x,"%s",LOCAL(i));
		//sprintf(x,"l_%d_%d",functionnum, i);
		TRY(DeclareVariable(F, x, i));
		IS_VARIABLE(i) = 1;
		free(x);
	}
	F->freeLocal = f->numparams;

	if (!IsMain(f)) {
		TRY(FunctionHeader(F));
		F->indent++;
	}

	// make function comment
	StringBuffer_printf(str, "-- function num : %s", funcnumstr);
	if (f->sizeupvalues > 0) {
		StringBuffer_add(str, " , upvalues : ");
		listUpvalues(f, str);
	}
	TRY(RawAddStatement(F, str));
	StringBuffer_prune(str);

	if (func_check == 1 && func_checking == 0) {
		int func_check_result = FunctionCheck(f, funcnumstr, str);
		TRY(RawAddStatement(F, str));
	}

	if ((f->is_vararg&1) && (f->is_vararg&2)) {
		TRY(DeclareVariable(F, "arg", F->freeLocal));
		F->freeLocal++;
	}

	if (locals) {
		for (i=F->freeLocal; i<f->maxstacksize; i++) {
			DeclareLocal(F,i,"nil");
		}
	}

	for (pc = n - 1; pc >= 0; pc--) {
		Instruction i = code[pc];
		OpCode o = GET_OPCODE(i);
		int sbc = GETARG_sBx(i);
		int dest = sbc + pc + 1;
		int real_end = GetJmpAddr(F,pc + 1);

		while (pc < F->loop_ptr->start) {
			F->loop_ptr = F->loop_ptr->parent;
		}

		if (o == OP_CLOSE) {
			int a = GETARG_A(i);
			AddToSet(F->do_opens, f->locvars[a].startpc);
			AddToSet(F->do_closes, f->locvars[a].endpc);
		} else if (o == OP_FORLOOP) {
			LoopItem* item = NewLoopItem(FORLOOP, dest-1, dest, dest, pc, real_end);
			AddToLoopTree(F, item);
		} else if (o == OP_JMP) {
			OpCode pc_1 = GET_OPCODE(code[pc-1]);

			AstStatement* jmp = NULL;
			AstStatement* jmpdest = cast(AstStatement*, F->jmpdests.tail);
			while (jmpdest && jmpdest->line > dest) {
				jmpdest = cast(AstStatement*, jmpdest->super.prev);
			}
			if (jmpdest == NULL || jmpdest->line < dest) {
				AstStatement* newjmpdest = MakeBlockStatement(JMP_DEST_STMT, NULL);
				newjmpdest->line = dest;
				AddAllAfterListItem(&(F->jmpdests), (ListItem*)jmpdest, (ListItem*)newjmpdest);
				jmpdest = newjmpdest;
			}
			jmp = MakeSimpleStatement(NULL);
			jmp->line = pc;
			jmp->parent = jmpdest;
			AddToListHead(jmpdest->sub, (ListItem*)jmp);

			if (dest == F->loop_ptr->out) {
				if (!isTestOpCode(pc_1)) {
					//breaks
					IntListItem* intItem = NewIntListItem(pc);
					AddToList(&(F->breaks), cast(ListItem*, intItem));
				}
			} else if (F->loop_ptr->start <= dest && dest < pc) {
				if (pc_1 == OP_TFORLOOP) {
					// TFORLOOP jump back
					LoopItem* item = NewLoopItem(TFORLOOP, dest-1, dest, dest, pc, real_end);
					AddToLoopTree(F, item);
				} else if (isTestOpCode(pc_1)) { //REPEAT jump back
					/* 
					** if the out loop(loop_ptr) is while and body=loop_ptr.start,
					** jump back may be 'until' or 'if', they are the same,
					** but 'if' is more clear, so we skip making a loop to choose 'if'.
					** see the lua code:
					** local a,b,c,f
					** 
					** while 1 do
					**	repeat
					**		f(b)
					**	until c
					**	f(a)
					** end
					** 
					** while 1 do
					**	f(b)
					** 	if c then
					** 		f(a)
					**	end
					** end
					*/
					if (!((F->loop_ptr->type == WHILE ) && (dest == F->loop_ptr->start))) {
						LoopItem* item = NewLoopItem(REPEAT, dest, dest, dest, pc, real_end);
						AddToLoopTree(F, item);
					}
				} else {
					// WHILE jump back
					LoopItem* item = NewLoopItem(WHILE, dest, dest, -1, pc, real_end);
					AddToLoopTree(F, item);
				}
			}
		}
	}

	F->loop_ptr = F->loop_tree;
	next_child = F->loop_tree->child;

	for (pc = 0; pc < n; pc++) {
		Instruction i = code[pc];
		OpCode o = GET_OPCODE(i);
		int a = GETARG_A(i);
		int b = GETARG_B(i);
		int c = GETARG_C(i);
		int bc = GETARG_Bx(i);
		int sbc = GETARG_sBx(i);


		F->pc = pc;

		if (pc > F->loop_ptr->end) {
			next_child = F->loop_ptr->next;
			F->loop_ptr = F->loop_ptr->parent;
		}

		while (next_child && pc >= next_child->body) {
			F->loop_ptr = next_child;
			next_child = F->loop_ptr->child;
		}

		// nil optimization of Lua 5.1
		if (pc == 0) {
			if ((o == OP_SETGLOBAL) || (o == OP_SETUPVAL)) {
				int ixx;
				for (ixx = F->freeLocal; ixx <= a; ixx++) {
					TRY(AssignReg(F, ixx, "nil", 0, 1));
					PENDING(ixx)=1;
				}
			} else if (o != OP_JMP) {
				int ixx;
				for (ixx = F->freeLocal; ixx <= a-1; ixx++) {
					TRY(AssignReg(F, ixx, "nil", 0, 1));
					PENDING(ixx)=1;
				}
			}
		}
		if (ignoreNext) {
			ignoreNext--;
			continue;
		}

		/*
		* Disassembler info
		*/
		if (debug) {
			fprintf(stddebug, "----------------------------------------------\n");
			fprintf(stddebug, "\t%d\t", pc + 1);
			fprintf(stddebug, "%-9s\t", luaP_opnames[o]);
			switch (getOpMode(o)) {
			case iABC:
				fprintf(stddebug, "%d %d %d", a, b, c);
				break;
			case iABx:
				fprintf(stddebug, "%d %d", a, bc);
				break;
			case iAsBx:
				fprintf(stddebug, "%d %d", a, sbc);
				break;
			}
			fprintf(stddebug, "\n");
		}

		TRY(DeclareLocals(F));
		TRY(ReleaseLocals(F));

		while (RemoveFromSet(F->do_opens, pc)) {
			AstStatement* blockstmt = MakeBlockStatement(BLOCK_STMT, NULL);
			AddAstStatement(F, cast(AstStatement*, blockstmt));
			F->currStmt = blockstmt;
		}

		while (RemoveFromSet(F->do_closes, pc)) {
			AstStatement* block = F->currStmt;
			if (block->type == BLOCK_STMT) {
				F->currStmt = block->parent;
			} else {
				SET_ERROR(F, "unexpected 'end' of 'do'");
			}
		}

		while ((F->currStmt->type == IF_THEN_STMT || F->currStmt->type == IF_ELSE_STMT)
			&& ElseStart(F->currStmt->parent) == GetJmpAddr(F, pc)) {
			AstStatement* ifstmt = F->currStmt->parent;
			F->currStmt = ifstmt->parent;
		}

		if (F->jmpdests.head && cast(AstStatement*, F->jmpdests.head)->line == pc) {
			AstStatement* jmpdest = cast(AstStatement*, RemoveFromList(&(F->jmpdests), F->jmpdests.head));
			AddToStatement(F->currStmt, jmpdest);
		}

		if ((F->loop_ptr->start == pc) && (F->loop_ptr->type == REPEAT || F->loop_ptr->type == WHILE)) {
			LoopItem* walk = F->loop_ptr;

			while (walk->parent && (walk->parent->start == pc ) &&(walk->parent->type == REPEAT || walk->parent->type == WHILE)) {
				walk = walk->parent;
			}

			while (!(walk == F->loop_ptr)) {
				AstStatement* loopstmt = NULL;
				if (walk->type == WHILE) {
					walk->body = walk->start;
					loopstmt = MakeBlockStatement(WHILE_STMT, strdup("1"));
				} else if (walk->type == REPEAT) {
					loopstmt = MakeBlockStatement(REPEAT_STMT, NULL);
				}
				RawAddAstStatement(F, cast(AstStatement*, loopstmt));
				F->currStmt = loopstmt;
				walk = walk->child;
			}

			if (walk->type == REPEAT) {
				AstStatement* loopstmt = MakeBlockStatement(REPEAT_STMT, NULL);
				RawAddAstStatement(F, cast(AstStatement*, loopstmt));
				F->currStmt = loopstmt;
			} else if (walk->type == WHILE) { 
				/*
				** try to process all while as " while 1 do if "
				** see the lua code:
				** local f, a, b, c
				** 
				** while test do
				** 	whilebody
				** end
				** 
				** while 1 do
				** 	if test then
				** 		whilebody
				** 	else
				** 		break
				** 	end
				** end
				*/
				AstStatement* loopstmt = MakeBlockStatement(WHILE_STMT, strdup("1"));
				RawAddAstStatement(F, cast(AstStatement*, loopstmt));
				F->currStmt = loopstmt;
				walk->body = walk->start;
			}
		}

		StringBuffer_prune(str);

		switch (o) {
		case OP_MOVE:
			/* Upvalue handling added to OP_CLOSURE */
			{
				const char* bstr = NULL;
				if (a == b)
					break;
				if (CALL(b) < 2)
					bstr = GetR(F, b);
				else
					UnsetPending(F, b);
				if (error)
					goto errorHandler;
				/*
				* Copy from one register to another
				*/
				TRY(AssignReg(F, a, bstr, PRIORITY(b), 1));
				break;
			}
		case OP_LOADK:
			{
				/*
				* Constant. Store it in register.
				*/
				char *ctt = DecompileConstant(f, bc);
				TRY(AssignReg(F, a, ctt, 0, 1));
				free(ctt);
				break;
			}
		case OP_LOADBOOL:
			{
				if ((F->bools.size == 0) || (c==0)) {
					/*
					* assign boolean constant
					*/
					if (PENDING(a)) {
						// some boolean constructs overwrite pending regs :(
						TRY(UnsetPending(F, a));
					}
					TRY(AssignReg(F, a, b ? "true" : "false", 0, 1));
				} else {
					/*
					* assign boolean value
					*/
					char *test = NULL;
					TRY(test = OutputBoolean(F, NULL, 1));
					StringBuffer_printf(str, "%s", test);
					if (test) free(test);
					TRY(AssignReg(F, a, StringBuffer_getRef(str), 0, 0));
				}
				if (c)
					ignoreNext = 1;
				break;
			}
		case OP_LOADNIL:
			{
				int i;
				/*
				* Read nil into register.
				*/
				for(i = a; i <= b; i++) {
					TRY(AssignReg(F, i, "nil", 0, 1));
				}
				break;
			}
		case OP_VARARG: // Lua5.1 specific.
			{
				int i;
				/*
				* Read ... into register.
				*/
				if (b==0) {
					TRY(AssignReg(F, a, "...", 0, 1));
					TRY(AssignReg(F, a+1, ".end", 0, 1));
				} else {
					for(i = 0; i < b-1; i++) {
						TRY(AssignReg(F, a+i, "...", 0, 1));
					}
				}
				break;
			}
		case OP_GETUPVAL:
			{
				TRY(AssignReg(F, a, UPVALUE(b), 0, 1));
				break;
			}
		case OP_GETGLOBAL:
			{
				/*
				* Read global into register.
				*/
				TRY(AssignReg(F, a, GLOBAL(bc), 0, 1));
				break;
			}
		case OP_GETTABLE:
			{
				/*
				* Read table entry into register.
				*/
				const char *bstr;
				char* cstr;
				TRY(cstr = RegisterOrConstant(F, c));
				TRY(bstr = GetR(F, b));
				if (isIdentifier(bstr)) {
					StringBuffer_set(str, bstr);
				} else {
					StringBuffer_printf(str, "(%s)", bstr);
				}
				MakeIndex(F, str, cstr, DOT);
				TRY(AssignReg(F, a, StringBuffer_getRef(str), 0, 0));
				free(cstr);
				break;
			}
		case OP_SETGLOBAL:
			{
				/*
				* Global Assignment statement.
				*/
				const char *var = GLOBAL(bc);
				const char *astr;
				TRY(astr = GetR(F, a));
				TRY(AssignGlobalOrUpvalue(F, var, astr));
				break;
			}
		case OP_SETUPVAL:
			{
				/*
				* Global Assignment statement.
				*/
				const char *var = UPVALUE(b);// UP(b) is correct
				const char *astr;
				TRY(astr = GetR(F, a));
				TRY(AssignGlobalOrUpvalue(F, var, astr));
				break;
			}
		case OP_SETTABLE:
			{
				const char *astr;
				char *bstr, *cstr;
				int settable;
				TRY(bstr = RegisterOrConstant(F, b));
				TRY(cstr = RegisterOrConstant(F, c));
				/*
				* first try to add into a table
				*/
				TRY(settable = SetTable(F, a, bstr, cstr));
				if (!settable) {
					/*
					* if failed, just output an assignment
					*/
					TRY(astr = GetR(F, a));
					if (isIdentifier(astr)) {
						StringBuffer_set(str, astr);
					} else {
						StringBuffer_printf(str, "(%s)", astr);
					}
					MakeIndex(F, str, bstr, DOT);
					TRY(AssignGlobalOrUpvalue(F, StringBuffer_getRef(str), cstr));
				}
				free(bstr);
				free(cstr);
				break;
			}
		case OP_NEWTABLE:
			{
				TRY(StartTable(F, a, b, c, pc));
				break;
			}
		case OP_SELF:
			{
				/*
				* Read table entry into register.
				*/
				const char *bstr;
				char *cstr;
				TRY(cstr = RegisterOrConstant(F, c));
				TRY(bstr = GetR(F, b));

				TRY(AssignReg(F, a+1, bstr, PRIORITY(b), 0));
				if (isIdentifier(bstr)) {
					StringBuffer_set(str, bstr);
				} else {
					StringBuffer_addPrintf(str, "(%s)", bstr);
				}
				MakeIndex(F, str, cstr, SELF);
				TRY(AssignReg(F, a, StringBuffer_getRef(str), 0, 0));
				free(cstr);
				break;
			}
		case OP_ADD:
		case OP_SUB:
		case OP_MUL:
		case OP_DIV:
		case OP_POW:
		case OP_MOD:
			{
				char *bstr, *cstr;
				const char *oper = operators[o];
				int prio = priorities[o];
				int bprio = PRIORITY(b);
				int cprio = PRIORITY(c);
				TRY(bstr = RegisterOrConstant(F, b));
				TRY(cstr = RegisterOrConstant(F, c));
				// FIXME: might need to change from <= to < here
				if ((prio != 1 && bprio <= prio) || (prio == 1 && bstr[0] != '-')) {
					StringBuffer_add(str, bstr);
				} else {
					StringBuffer_addPrintf(str, "(%s)", bstr);
				}
				StringBuffer_addPrintf(str, " %s ", oper);
				// FIXME: being conservative in the use of parentheses
				if (cprio < prio) {
					StringBuffer_add(str, cstr);
				} else {
					StringBuffer_addPrintf(str, "(%s)", cstr);
				}
				TRY(AssignReg(F, a, StringBuffer_getRef(str), prio, 0));
				free(bstr);
				free(cstr);
				break;
			}
		case OP_UNM:
		case OP_NOT:
		case OP_LEN:
			{
				const char *bstr;
				int prio = priorities[o];
				int bprio = PRIORITY(b);
				TRY(bstr = GetR(F, b));
				StringBuffer_add(str, operators[o]);
				if (bprio <= prio) {
					StringBuffer_add(str, bstr);
				} else {
					StringBuffer_addPrintf(str, "(%s)", bstr);
				}
				TRY(AssignReg(F, a, StringBuffer_getRef(str), 0, 0));
				break;
			}
		case OP_CONCAT:
			{
				int i;
				for (i = b; i <= c; i++) {
					const char *istr;
					TRY(istr = GetR(F, i));
					if (PRIORITY(i) > priorities[o]) {
						StringBuffer_addPrintf(str, "(%s)", istr);
					} else {
						StringBuffer_add(str, istr);
					}
					if (i < c)
						StringBuffer_add(str, " .. ");
				}
				TRY(AssignReg(F, a, StringBuffer_getRef(str), 0, 0));
				break;
			}
		case OP_JMP:
			{
				int dest = sbc + pc + 2;
				Instruction idest = code[dest - 1];
				IntListItem* foundInt = (IntListItem*)RemoveFromList(&(F->breaks), FindFromListTail(&(F->breaks), (ListItemCmpFn)MatchIntListItem, &pc));
				if (foundInt != NULL){ // break
					free(foundInt);
					StringBuffer_printf(str, "do break end");
					TRY(AddStatement(F, str));
				}else if (F->loop_ptr->end == pc){ // until jmp has been processed, tforloop has ignored the jmp, forloop does not have a jmp
					if (F->currStmt->type == IF_THEN_STMT && ElseStart(F->currStmt->parent) == GetJmpAddr(F, pc + 1)) {
						// Change 'while 1 do if' to 'while'
						AstStatement* currStmt = F->currStmt;
						AstStatement* ifStmt = currStmt->parent;
						AstStatement* parentStmt = ifStmt->parent;
						if (parentStmt->type == WHILE_STMT && parentStmt->sub->size == 1) {
							// if is the first statment of while body
							AstStatement* whileStmt = parentStmt;
							char* whileTest = whileStmt->code;
							if (strcmp(whileTest, "1") == 0) {
								// ifthen to while
								RemoveFromList(ifStmt->sub, (ListItem*)currStmt);
								currStmt->type = WHILE_STMT;
								currStmt->code = ifStmt->code;
								currStmt->line = ifStmt->line;
								currStmt->parent = whileStmt->parent;
								RemoveFromList(whileStmt->parent->sub, (ListItem*)whileStmt);
								AddToStatement(currStmt->parent, currStmt);
								ifStmt->code = NULL;
								DeleteAstStatement(whileStmt);
							}
						}
					}
					if (F->currStmt->type == WHILE_STMT) {
						F->currStmt = F->currStmt->parent;
					} else {
						SET_ERROR(F, "unexpected 'end' of 'while'");
					}
				}else if (F->currStmt->type == IF_THEN_STMT && ElseStart(F->currStmt->parent) == GetJmpAddr(F, pc + 1)) { // jmp before 'else'
					AstStatement* ifstmt = F->currStmt->parent;
					F->currStmt = ElseStmt(ifstmt);
					ElseStart(ifstmt) = GetJmpAddr(F, dest);
				}else if (GET_OPCODE(idest) == OP_TFORLOOP) { // jmp of generic for
					/*
					* generic 'for'
					*/
					int i;
					//int step;
					const char *generator, *control, *state;
					//char *variables[20];
					const char* vname[40];
					AstStatement* forstmt = NULL;
					//int stepLen;

					a = GETARG_A(idest);
					c = GETARG_C(idest);

					F->intspos++;
					generator = GetR(F, a);
					control = GetR(F, a + 2);
					state = GetR(F, a + 1);
					for (i=1; i<=c; i++) {
						if (!IS_VARIABLE(a+2+i)) {
							int i2;
							int loopvars = 0;
							vname[i-1] = NULL;
							for (i2 = 0; i2 < f->sizelocvars; i2++) {
								if (f->locvars[i2].startpc == pc + 1) {
									loopvars++;
									//search for the loop variable. Set it's endpc one step further so it will be the same for all loop variables
									if (GET_OPCODE(F->f->code[f->locvars[i2].endpc-2]) == OP_TFORLOOP) {
										f->locvars[i2].endpc -= 2;
									}
									if (GET_OPCODE(F->f->code[f->locvars[i2].endpc-1]) == OP_TFORLOOP) {
										f->locvars[i2].endpc -= 1;
									}
									if (loopvars==3+i) {
										vname[i-1] = LOCAL(i2);
										break;
									}
								}
							}
							if (vname[i-1] == NULL) {
								char tmp[5];
								sprintf(tmp,"i_%d",i);
								TRY(DeclareVariable(F, tmp, a+2+i));
								vname[i-1] = F->R[a+2+i];
							}
						} else {
							vname[i-1] = F->R[a+2+i];
						}
						F->Rinternal[a+2+i] = 1;
					}

					DeclarePendingLocals(F);

					StringBuffer_printf(str,"%s",vname[0]);
					for (i=2; i<=c; i++) {
						StringBuffer_addPrintf(str, ",%s",vname[i-1]);
					}
					StringBuffer_addPrintf(str," in ");
					StringBuffer_addPrintf(str,"%s",generator);

					F->Rinternal[a] = 1;
					F->Rinternal[a + 1] = 1;
					F->Rinternal[a + 2] = 1;

					F->intbegin[F->intspos] = a;
					F->intend[F->intspos] = a+2+c;
					forstmt = MakeBlockStatement(TFORLOOP_STMT, StringBuffer_getBuffer(str));
					AddAstStatement(F, forstmt);
					F->currStmt = forstmt;
					break;
				} else if (sbc == 2 && GET_OPCODE(code[pc+2]) == OP_LOADBOOL) {
					/*
					* JMP 2
					* LOADBOOL skip next , must be this and cannot be excuted, will be treat as TESTSET
					* LOADBOOL
					* ::jmp_target
					*/
					fprintf(stderr, "processing OP_JMP to } else if (sbc == 2 && GET_OPCODE(code[pc+2]) == OP_LOADBOOL) { \n");
					fprintf(stderr, " at line %d in file %s\n", __LINE__, __FILE__);
					fprintf(stderr, " for lua files: ");
					printFileNames(stderr);
					fprintf(stderr, "\n");
					fprintf(stderr, " at lua function %s pc=%d\n\n", funcnumstr, pc);
					fflush(stderr);
					{
					int boola = GETARG_A(code[pc+1]);
					char* test = NULL;
					/* skip */
					const char* ra = REGISTER(boola);
					AddToList(&(F->bools), (ListItem*)MakeBoolOp(luadec_strdup(ra), luadec_strdup(ra), OP_TESTSET, c, pc+3, dest));
					F->testpending = a+1;
					F->testjump = dest;
					TRY(test = OutputBoolean(F, NULL, 1));
					StringBuffer_printf(str, "%s", test);
					if (test) free(test);
					TRY(UnsetPending(F, boola));
					TRY(AssignReg(F, boola, StringBuffer_getRef(str), 0, 0));
					ignoreNext = 2;
					}
				} else if (GET_OPCODE(idest) == OP_LOADBOOL) { // WHY
					/*
					* constant boolean value
					* JMP 
					* ....skipped , not decompiled
					* ::jmp_target
					* LOADBOOL
					*/
					fprintf(stderr, "processing OP_JMP to } else if (GET_OPCODE(idest) == OP_LOADBOOL) { \n");
					fprintf(stderr, " at line %d in file %s\n", __LINE__, __FILE__);
					fprintf(stderr, " for lua files: ");
					printFileNames(stderr);
					fprintf(stderr, "\n");
					fprintf(stderr, " at lua function %s pc=%d\n\n", funcnumstr, pc);
					fflush(stderr);
					pc = dest - 2;
				} else if (sbc == 0) {
					/* dummy jump -- ignore it */
					break;
				} else { // WHY
					fprintf(stderr, "processing OP_JMP to } else { \n");
					fprintf(stderr, " at line %d in file %s\n", __LINE__, __FILE__);
					fprintf(stderr, " for lua files: ");
					printFileNames(stderr);
					fprintf(stderr, "\n");
					fprintf(stderr, " at lua function %s pc=%d\n\n", funcnumstr, pc);
					fflush(stderr);
					{
					int nextpc = pc+1;
					int nextsbc = sbc-1;
					for (;;) {
						Instruction nextins = code[nextpc];
						if (GET_OPCODE(nextins) == OP_JMP && GETARG_sBx(nextins) == nextsbc) {
							nextpc++;
							nextsbc--;
						} else
							break;
						if (nextsbc == -1) {
							break;
						}
					}
					if (nextsbc == -1) {
						pc = nextpc-1;
						break;
					}
					if (F->indent > baseIndent) {
						StringBuffer_printf(str, "do return end");
					} else {
						pc = dest-2;
					}
					TRY(AddStatement(F, str));
					}
				}
				break;
			}
		case OP_EQ:
		case OP_LT:
		case OP_LE:
			{
				if (IS_CONSTANT(b)) {
					int swap = b;
					b = c;
					c = swap;
					a = !a;
					if (o == OP_LT) o = OP_LE;
					else if (o == OP_LE) o = OP_LT;
				}
				AddToList(&(F->bools), (ListItem*)MakeBoolOp(RegisterOrConstant(F, b), RegisterOrConstant(F, c), o, a, pc+1, -1));
				goto LOGIC_NEXT_JMP;
				break;
			}
		case OP_TESTSET: // Lua5.1 specific TODO: correct it
		case OP_TEST:
			{
				int cmpa, cmpb, cmpc;
				const char *ra, *rb;

				if (o==OP_TESTSET) {
					cmpa = a;
					cmpb = b;
					cmpc = c;
				} else {
					cmpa = a;
					cmpb = a;
					cmpc = c;
					// StringBuffer_add(str, "  -- Lua5.1 code: CHECK");
					// TRY(AddStatement(F, str));
				}

				if (!IS_VARIABLE(cmpa)) {
					ra = REGISTER(cmpa);
					TRY(rb = GetR(F, cmpb));
					PENDING(cmpa) = 0;
				} else {
					TRY(ra = GetR(F, cmpa));
					if (cmpa != cmpb) {
						TRY(rb = GetR(F, cmpb));
					} else {
						rb = ra;
					}
				}
				AddToList(&(F->bools), (ListItem*)MakeBoolOp(luadec_strdup(ra), luadec_strdup(rb), o, cmpc, pc+1, -1));
				// Within an IF, a and b are the same, avoiding side-effects
				if (cmpa != cmpb || !IS_VARIABLE(cmpa)) {
					F->testpending = cmpa+1;
				}
				goto LOGIC_NEXT_JMP;
				break;
			}
LOGIC_NEXT_JMP:
			{
				int dest;
				BoolOp* lastBool;
				pc++;
				F->pc = pc;
				i = code[pc];
				o = GET_OPCODE(i);
				if (o != OP_JMP) {
					assert(0);
				}
				sbc = GETARG_sBx(i);
				dest = sbc + pc + 2;
				lastBool = cast(BoolOp*, LastItem(&(F->bools)));
				lastBool->dest = dest;
				if (F->testpending) {
					F->testjump = dest;
				}
				if (( F->loop_ptr->type == REPEAT) && (F->loop_ptr->end == F->pc )) {
					int endif, thenaddr;
					char* test = NULL;
					LogicExp* exp = NULL;
					TRY(exp = MakeBoolean(F, &thenaddr, &endif));
					TRY(test = WriteBoolean(exp, &thenaddr, &endif, 0));
					if (F->currStmt->type == REPEAT_STMT) {
						F->currStmt->code = test;
						test = NULL;
						F->currStmt = F->currStmt->parent;
					} else {
						SET_ERROR(F, "unexpected 'until' of 'repeat'");
					}
					if (test) free(test);
					if (exp) DeleteLogicExpTree(exp);
				}
				break;
			}
		case OP_CALL:
		case OP_TAILCALL:
			{
				/*
				* Function call. The CALL opcode works like this:
				* R(A),...,R(A+F-2) := R(A)(R(A+1),...,R(A+B-1))
				*/
				int i, limit, self;
				const char* astr;
				self = 0;

				if (b == 0) {
					limit = a + 1;
					while (PENDING(limit) || IS_VARIABLE(limit)) limit++;
				} else {
					limit = a + b;
				}
				StringBuffer_prune(str);
				if (o == OP_TAILCALL) {
					StringBuffer_set(str, "return ");
					ignoreNext = 1;
				}
				TRY(astr = GetR(F, a));
				if (isIdentifier(astr)) {
					StringBuffer_addPrintf(str, "%s(", astr);
				} else {
					const char* at = astr + strlen(astr) - 1;
					while (at > astr && (luadec_isalnum(*at) || *at == '_')) {
						at--;
					}
					if (*at == ':') {
						self = 1;
						StringBuffer_addPrintf(str, "%s(", astr);
					} else {
						StringBuffer_addPrintf(str, "(%s)(", astr);
					}
				}

				for (i = a + 1; i < limit; i++) {
					const char *ireg = NULL;
					TRY(ireg = GetR(F, i));
					if(strcmp(ireg,".end") == 0)
						break;
					if (self && i == a+1)
						continue;
					if (i > a + 1 + self)
						StringBuffer_add(str, ", ");
					if (ireg)
						StringBuffer_add(str, ireg);
				}
				StringBuffer_addChar(str, ')');

				if (c == 0) {
					F->lastCall = a;
				}
				if (GET_OPCODE(code[pc+1]) == OP_LOADNIL && GETARG_A(code[pc+1]) == a+1) {
					StringBuffer_prepend(str, "(");
					StringBuffer_add(str, ")");
					c += GETARG_B(code[pc+1]) - GETARG_A(code[pc+1]) + 1;
					// ignoreNext = 1;
				}
				if (o == OP_TAILCALL || c == 1 ) {
					TRY(AddStatement(F, str));
				} else {
					TRY(AssignReg(F, a, StringBuffer_getRef(str), 0, 0));
					if (c == 0) TRY(AssignReg(F, a+1, ".end", 0, 1));
					for (i = 0; i < c-1; i++) {
						CALL(a+i) = i+1;
					}
				}
				break;
			}
		case OP_RETURN:
			{
				/*
				* Return call. The RETURN opcode works like this: return
				* R(A),...,R(A+B-2)
				*/
				int i, limit;

				/* skip the last RETURN */
				if (pc == n - 1)
					break;
				if (b == 0) {
					limit = a;
					while (PENDING(limit) || IS_VARIABLE(limit)) limit++;
				}
				else
					limit = a + b - 1;
				StringBuffer_set(str, "return ");
				for (i = a; i < limit; i++) {
					const char* istr = GetR(F, i);
					if (strcmp(istr,".end") == 0)
						break;
					if (i > a)
						StringBuffer_add(str, ", ");
					TRY(StringBuffer_add(str, istr));
				}
				TRY(AddStatement(F, str));
				break;
			}
		case OP_FORLOOP: //Lua5.1 specific. TODO: CHECK
			{
				int i;
				AstStatement* currStmt = F->currStmt;

				for (i=F->intbegin[F->intspos]; i<=F->intend[F->intspos]; i++)
				{
					//fprintf(stderr,"X %d\n",i);
					IS_VARIABLE(i)=0;
					F->Rinternal[i] = 0;
				}
				F->intspos--;
				F->ignore_for_variables = 0;

				if (currStmt->type == FORLOOP_STMT) {
					F->currStmt = currStmt->parent;
				} else {
					SET_ERROR(F, "unexpected 'end' of 'for' loop");
				}
				break;
			}
		case OP_TFORLOOP: //Lua5.1 specific. TODO: CHECK
			{
				int i;
				AstStatement* currStmt = F->currStmt;
				for (i=F->intbegin[F->intspos]; i<=F->intend[F->intspos]; i++)
				{
					IS_VARIABLE(i)=0;
					F->Rinternal[i] = 0;
				}
				F->intspos--;

				F->ignore_for_variables = 0;
				if (currStmt->type == TFORLOOP_STMT) {
					F->currStmt = currStmt->parent;
				} else {
					SET_ERROR(F, "unexpected 'end' of generic 'for' loop");
				}
				ignoreNext = 1;
				break;
			}
		case OP_FORPREP: //Lua5.1 specific. TODO: CHECK
			{
				/*
				* numeric 'for'
				*/
				int i;
				int step;
				const char *idxname = NULL;
				const char *initial, *a1str, *endstr;
				int stepLen;
				AstStatement* forstmt = NULL;
				F->intspos++;
				TRY(initial = GetR(F, a));
				TRY(endstr = GetR(F, a+2));
				TRY(a1str = GetR(F, a+1));

				if (!IS_VARIABLE(a+3)) {
					int loopvars = 0;
					for (i = 0; i < f->sizelocvars; i++) {
						if (f->locvars[i].startpc == pc + 1) {
							loopvars++;
							//search for the loop variable. Set it's endpc one step further so it will be the same for all loop variables
							if (GET_OPCODE(F->f->code[f->locvars[i].endpc-1]) == OP_FORLOOP) {
								f->locvars[i].endpc -=1;
							}
							if (loopvars==4) {
								idxname = LOCAL(i);
								break;
							}
						}
					}
					if (idxname == NULL) {
						idxname = "i";
						TRY(DeclareVariable(F, idxname, a + 3));
					}
				} else {
					idxname = F->R[a+3];
				}
				DeclarePendingLocals(F);
				/*
				* if A argument for FORLOOP is not a known variable,
				* it was declared in the 'for' statement. Look for
				* its name in the locals table.
				*/



				//initial = luadec_strdup(initial);
				step = atoi(REGISTER(a + 2));
				stepLen = strlen(REGISTER(a + 2));
				// findSign = strrchr(initial, '-');

				// if (findSign) {
				//    initial[strlen(initial) - stepLen - 3] = '\0';
				// }

				if (step == 1) {
					StringBuffer_printf(str, "%s = %s, %s",
						idxname, initial, a1str);
				} else {
					StringBuffer_printf(str, "%s = %s, %s, %s",
						idxname, initial, a1str, REGISTER(a + 2));
				}

				/*
				* Every numeric 'for' declares 4 variables.
				*/
				F->Rinternal[a] = 1;
				F->Rinternal[a + 1] = 1;
				F->Rinternal[a + 2] = 1;
				F->Rinternal[a + 3] = 1;
				F->intbegin[F->intspos] = a;
				F->intend[F->intspos] = a+3;
				forstmt = MakeBlockStatement(FORLOOP_STMT, StringBuffer_getBuffer(str));
				AddAstStatement(F, forstmt);
				F->currStmt = forstmt;
				break;
			}
		case OP_SETLIST:
			{
				TRY(SetList(F, a, b, c));
				break;
			}
		case OP_CLOSE:
			/*
			* Handled in do_opens/do_closes variables.
			*/
			break;
		case OP_CLOSURE:
			{
				/*
				* Function.
				*/
				int i;
				int uvn;
				int cfnum = functionnum;

				uvn = f->p[c]->nups;

				/* determining upvalues */

				// upvalue names = next n opcodes after CLOSURE

				if (!f->p[c]->upvalues) {
					f->p[c]->sizeupvalues = uvn;
					f->p[c]->upvalues = luaM_newvector(glstate,uvn,TString*);

					for (i=0; i<uvn; i++) {
						if (GET_OPCODE(code[pc+i+1]) == OP_MOVE) {
							char names[10];
							sprintf(names,"l_%d_%d",functionnum,GETARG_B(code[pc+i+1]));
							f->p[c]->upvalues[i] = luaS_new(glstate, names);
						} else if (GET_OPCODE(code[pc+i+1]) == OP_GETUPVAL) {
							f->p[c]->upvalues[i] = f->upvalues[GETARG_B(code[pc+i+1])];
						} else {
							char names[20];
							sprintf(names,"upval_%d_%d",functionnum,i);
							f->p[c]->upvalues[i] = luaS_new(glstate, names);
						}
					}
				}
				/* upvalue determinition end */

				if ( func_checking == 1){
					char* code = NULL;
					char* newfuncnumstr = (char*)calloc(strlen(funcnumstr) + 10, sizeof(char));
					functionnum = c;
					sprintf(newfuncnumstr, "%s_%d", funcnumstr, c);
					code = PrintFunctionOnlyParamsAndUpvalues(f->p[c], F->indent, newfuncnumstr);
					StringBuffer_setBuffer(str, code);
				}else if (!process_sub){
					StringBuffer_printf(str, "DecompiledFunction_%s_%d", funcnumstr, c);
				}else{
					char* code = NULL;
					char* newfuncnumstr = (char*)calloc(strlen(funcnumstr) + 10, sizeof(char));
					functionnum = c;
					sprintf(newfuncnumstr, "%s_%d", funcnumstr, c);
					code = ProcessCode(f->p[c], F->indent, 0, newfuncnumstr);
					StringBuffer_setBuffer(str, code);
				}
				TRY(AssignReg(F, a, StringBuffer_getRef(str), 0, 0));
				/* need to add upvalue handling */

				ignoreNext = f->p[c]->sizeupvalues;

				break;
			}
		default:
			StringBuffer_printf(str, "-- unhandled opcode? : %-9s", luaP_opnames[o]);
			TRY(AddStatement(F, str));
			break;
		}

		if (debug) {
			TRY(ShowState(F));
		}

		// TODO Check, should not do this
		if ((F->currStmt->type == IF_THEN_STMT || F->currStmt->type == IF_THEN_STMT)
			&& ElseStart(F->currStmt->parent) == GetJmpAddr(F, pc - 1)) {
			AstStatement* ifstmt = F->currStmt->parent;
			F->currStmt = ifstmt->parent;
		}

		TRY(OutputAssignments(F));
	}

	// TODO Check, should not do this
	if ((F->currStmt->type == IF_THEN_STMT || F->currStmt->type == IF_THEN_STMT)
		&& ElseStart(F->currStmt->parent) == GetJmpAddr(F, pc)) {
		AstStatement* ifstmt = F->currStmt->parent;
		F->currStmt = ifstmt->parent;
	}

	TRY(FlushBoolean(F));

	if (SET_SIZE(F->tpend)>0) {
		StringBuffer_set(str, "-- WARNING: undefined locals caused missing assignments!");
		TRY(AddStatement(F, str));
	}

	if (F->jmpdests.size > 0) {
		StringBuffer_printf(str, "-- DECOMPILER ERROR: %d unprocessed JMP targets", F->jmpdests.size);
		TRY(AddStatement(F, str));
		while (F->jmpdests.head) {
			AstStatement* jmpdest = cast(AstStatement*, RemoveFromList(&(F->jmpdests), F->jmpdests.head));
			AddToStatement(F->currStmt, jmpdest);
		}
	}

	if (!IsMain(f)) {
		F->indent--;
	}

	output = PrintFunction(F);
	DeleteFunction(F);
	StringBuffer_delete(str);
	return output;

errorHandler:
	printf("ERRORHANDLER\n");
	{
		AstStatement* stmt;
		StringBuffer_printf(str, "--[[ DECOMPILER ERROR %d: %s ]]", errorCode, error);
		stmt = MakeSimpleStatement(StringBuffer_getBuffer(str));
		stmt->line = F->pc;
		AddToStatement(F->currStmt, stmt);
		F->lastLine = F->pc;
	}
	output = PrintFunction(F);
	DeleteFunction(F);
	error = NULL;
	StringBuffer_delete(str);
	return output;
}

void luaU_decompile(Proto* f, int dflag) {
	char* code;
	debug = dflag;
	functionnum = 0;
	errorStr = StringBuffer_new(NULL);
	code = ProcessCode(f, 0, 0, luadec_strdup("0"));
	StringBuffer_delete(errorStr);
	printf("%s\n", code);
	free(code);
	fflush(stdout);
	fflush(stderr);
}

Proto* findSubFunction(Proto* f, const char* funcnumstr, char* realfuncnumstr) {
	Proto* cf = f;
	const char* startstr = funcnumstr;
	const char* endstr;

	int c = atoi(startstr);
	if (c != 0) {
		return NULL;
	}
	endstr = strchr(startstr, '_');
	startstr = endstr + 1;
	sprintf(realfuncnumstr, "0");
	functionnum = 0;

	while (!(endstr == NULL)) {
		c = atoi(startstr);
		if (c < 0 || c >= cf->sizep) {
			return NULL;
		}
		cf = cf->p[c];
		endstr = strchr(startstr, '_');
		startstr = endstr + 1;
		sprintf(realfuncnumstr + strlen(realfuncnumstr), "_%d", c);
		functionnum = c + 1;
	}
	return cf;
}

char* ProcessSubFunction(Proto* cf, int func_checking, char* funcnumstr) {
	int i;
	int uvn = cf->nups;
	char* code;
	StringBuffer* buff = StringBuffer_newBySize(128);

	/* determining upvalues */

	// upvalue names = next n opcodes after CLOSURE

	if (!cf->upvalues) {
		cf->sizeupvalues = uvn;
		cf->upvalues = luaM_newvector(glstate, uvn, TString*);

		for (i = 0; i<uvn; i++) {
			char names[10];
			sprintf(names, "l_%d_%d", 0, i);
			cf->upvalues[i] = luaS_new(glstate, names);
		}
	}

	if (!IsMain(cf)) {
		StringBuffer_set(buff, "local ");
		listUpvalues(cf, buff);
		StringBuffer_addPrintf(buff, "\nDecompiledFunction_%s = ", funcnumstr);
	}
	code = ProcessCode(cf, 0, func_checking, funcnumstr);
	StringBuffer_addPrintf(buff, "%s\n", code);
	free(code);
	code = StringBuffer_getBuffer(buff);
	StringBuffer_delete(buff);
	return code;
}

void luaU_decompileSubFunction(Proto* f, int dflag, const char* funcnumstr) {
	char* code;
	char* realfuncnumstr = (char*)calloc(strlen(funcnumstr) + 10, sizeof(char));

	Proto* cf = findSubFunction(f, funcnumstr, realfuncnumstr);
	if (cf == NULL) {
		fprintf(stderr, "No such sub function num : %s , use -pn option to get available num.\n", funcnumstr);
		free(realfuncnumstr);
		return;
	}

	errorStr = StringBuffer_new(NULL);
	code = ProcessSubFunction(cf, 0, realfuncnumstr);
	StringBuffer_delete(errorStr);
	printf("%s\n", code);
	free(code);
	fflush(stdout);
	fflush(stderr);
}
