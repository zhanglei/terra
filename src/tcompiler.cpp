#include "tcompiler.h"
#include "terra.h"
extern "C" {
#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"
}
#include <assert.h>
#include <stdio.h>

#include "llvm/DerivedTypes.h"
#include "llvm/ExecutionEngine/ExecutionEngine.h"
#include "llvm/ExecutionEngine/JIT.h"
#include "llvm/LLVMContext.h"
#include "llvm/Module.h"
#include "llvm/PassManager.h"
#include "llvm/Analysis/Verifier.h"
#include "llvm/Analysis/Passes.h"
#include "llvm/Target/TargetData.h"
#include "llvm/Transforms/Scalar.h"
#include "llvm/Support/IRBuilder.h"
#include "llvm/Support/TargetSelect.h"
using namespace llvm;

struct terra_CompilerState {
	Module * m;
	LLVMContext * ctx;
	ExecutionEngine * ee;
	FunctionPassManager * fpm;
};

static int terra_compile(lua_State * L);  //entry point from lua into compiler

void terra_compilerinit(struct terra_State * T) {
	lua_getfield(T->L,LUA_GLOBALSINDEX,"terra");
	lua_pushlightuserdata(T->L,(void*)T);
	lua_pushcclosure(T->L,terra_compile,1);
	lua_setfield(T->L,-2,"compile");
	lua_pop(T->L,1); //remove terra from stack
	T->C = (terra_CompilerState*) malloc(sizeof(terra_CompilerState));
	InitializeNativeTarget();
	
	T->C->ctx = &getGlobalContext();
	T->C->m = new Module("terra",*T->C->ctx);
	std::string err;
	T->C->ee = EngineBuilder(T->C->m).setErrorStr(&err).create();
	if (!T->C->ee) {
		terra_reporterror(T,"llvm: %s\n",err.c_str());
	}
	T->C->fpm = new FunctionPassManager(T->C->m);
	
	//TODO: add optimization passes here, these are just from llvm tutorial and are probably not good
	T->C->fpm->add(new TargetData(*T->C->ee->getTargetData()));
	// Provide basic AliasAnalysis support for GVN.
	T->C->fpm->add(createBasicAliasAnalysisPass());
	// Promote allocas to registers.
	T->C->fpm->add(createPromoteMemoryToRegisterPass());
	// Do simple "peephole" optimizations and bit-twiddling optzns.
	T->C->fpm->add(createInstructionCombiningPass());
	// Reassociate expressions.
	T->C->fpm->add(createReassociatePass());
	// Eliminate Common SubExpressions.
	T->C->fpm->add(createGVNPass());
	// Simplify the control flow graph (deleting unreachable blocks, etc).
	T->C->fpm->add(createCFGSimplificationPass());
	
	T->C->fpm->doInitialization();
}
//object to hold reference to lua object and help extract information
struct Obj {
	Obj() {
		ref = LUA_NOREF; L = NULL;
	}
	void initFromStack(lua_State * L, int ref_table) {
		freeref();
		this->L = L;
		this->ref_table = ref_table;
		assert(!lua_isnil(this->L,-1));
		this->ref = luaL_ref(this->L,this->ref_table);
	}
	~Obj() {
		freeref();
	}
	int size() {
		push();
		int i = lua_objlen(L,-1);
		pop();
		return i;
	}
	void objAt(int i, Obj * r) {
		push();
		lua_rawgeti(L,-1,i);
		r->initFromStack(L,ref_table);
		pop();
	}
	double number(const char * field) {
		push();
		lua_getfield(L,-1,field);
		double r = lua_tonumber(L,-1);
		pop(2);
		return r;
	}
	int64_t integer(const char * field) {
		push();
		lua_getfield(L,-1,field);
		const void * ud = lua_touserdata(L,-1);
		pop(2);
		int64_t i = *(const int64_t*)ud;
		return i;
	}
	const char * string(const char * field) {
		push();
		lua_getfield(L,-1,field);
		const char * r = luaL_checkstring(L,-1);
		pop(2);
		return r;
	}
	bool obj(const char * field, Obj * r) {
		push();
		lua_getfield(L,-1,field);
		if(lua_isnil(L,-1)) {
			pop(2);
			return false;
		} else {
			r->initFromStack(L,ref_table);
			pop();
			return true;
		}
	}
	void * ud(const char * field) {
		push();
		lua_getfield(L,-1,field);
		void * u = lua_touserdata(L,-1);
		pop(2);
		return u;
	}
	void pushfield(const char * field) {
		push();
		lua_getfield(L,-1,field);
		lua_remove(L,-2);
	}
	void push() {
		//fprintf(stderr,"getting %d %d\n",ref_table,ref);
		assert(lua_gettop(L) >= ref_table);
		lua_rawgeti(L,ref_table,ref);
	}
	void begin_match(const char * field) {
		pushfield(field);
	}
	int matches(const char * key) {
		lua_pushstring(L,key);
		int e = lua_rawequal(L,-1,-2);
		pop();
		if(e) {
			end_match();
		}
		return e;
	}
	void end_match() {
		pop();
	}
	void setfield(const char * key) { //sets field to value on top of the stack and pops it off
		push();
		lua_pushvalue(L,-2);
		lua_setfield(L,-2,key);
		pop(2);
	}
	void dump() {
		int n = lua_gettop(L);
		for (int i = 1; i <= n; i++) {
			printf("%d: (%s) %s\n",i,lua_typename(L,lua_type(L,i)),lua_tostring(L, i));
		}
	}
private:
	void freeref() {
		if(ref != LUA_NOREF) {
			luaL_unref(L,ref_table,ref);
			L = NULL;
			ref = LUA_NOREF;
		}
	}
	void pop(int n = 1) {
		lua_pop(L,n);
	}
	int ref;
	int ref_table;
	lua_State * L; 
};

static void terra_compile_llvm(terra_State * T) {
	Obj func;

}

struct TType { //contains llvm raw type pointer and any metadata about it we need
	Type * type;
	bool issigned;
	bool islogical;
};

struct Compiler {
	lua_State * L;
	terra_State * T;
	terra_CompilerState * C;
	IRBuilder<> * B;
	BasicBlock * BB; //current basic block
	Obj funcobj;
	Function * func;
	TType * getType(Obj * typ) {
		TType * t = (TType*) typ->ud("llvm_type");
		if(t == NULL) {
			t = (TType*) lua_newuserdata(T->L,sizeof(TType));
			memset(t,0,sizeof(TType));
			typ->setfield("llvm_type");
			typ->begin_match("kind");
			if(typ->matches("builtin")) {
				int bytes = typ->number("bytes");
				typ->begin_match("type");
				if(typ->matches("float")) {
					if(bytes == 4) {
						t->type = Type::getFloatTy(*C->ctx);
					} else {
						assert(bytes == 8);
						t->type = Type::getDoubleTy(*C->ctx);
					}
				} else if(typ->matches("integer")) {
					t->issigned = typ->number("signed");
					t->type = Type::getIntNTy(*C->ctx,bytes * 8);
				} else if(typ->matches("logical")) {
					t->type = Type::getInt8Ty(*C->ctx);
					t->islogical = true;
				} else {
					terra_reporterror(T,"type not understood");
					typ->end_match();
				}
			} else if(typ->matches("pointer")) {
				Obj base;
				typ->obj("type",&base);
				t->type = PointerType::getUnqual(getType(&base)->type);
			} else if(typ->matches("functype")) {
				std::vector<Type*> arguments;
				Obj params,rets;
				typ->obj("parameters",&params);
				typ->obj("returns",&rets);
				int sz = rets.size();
				Type * rt; 
				if(sz == 0) {
					rt = Type::getVoidTy(*C->ctx);
				} else if(sz == 1) {
					Obj r0;
					rets.objAt(1,&r0);
					TType * r0t = getType(&r0);
					rt = r0t->type;
				} else {
					terra_reporterror(T,"NYI - multiple returns");
				}
				int psz = params.size();
				for(int i = 1; i <= psz; i++) {
					Obj p;
					params.objAt(i,&p);
					TType * pt = getType(&p);
					arguments.push_back(pt->type);
				}
				t->type = FunctionType::get(rt,arguments,false); 
			} else {
				typ->end_match();
				terra_reporterror(T,"type not understood");
			}
			assert(t && t->type);
		}
		return t;
	}
	TType * typeOfValue(Obj * v) {
		Obj t;
		v->obj("type",&t);
		return getType(&t);
	}
	AllocaInst * allocVar(Obj * v) {
		IRBuilder<> TmpB(&func->getEntryBlock(),
                          func->getEntryBlock().begin()); //make sure alloca are at the beginning of the function
                                                          //TODO: is this really needed? this is what llvm example code does...
		AllocaInst * a = TmpB.CreateAlloca(typeOfValue(v)->type,0,v->string("name"));
		lua_pushlightuserdata(L,a);
		v->setfield("value");
		return a;
	}
	void run(terra_State * _T) {
		T = _T;
		L = T->L;
		C = T->C;
		B = new IRBuilder<>(*C->ctx);
		//create lua table to hold object references anchored on stack
		lua_newtable(T->L);
		int ref_table = lua_gettop(T->L);
		lua_pushvalue(T->L,-2); //the original argument
		funcobj.initFromStack(T->L, ref_table);
		
		Obj typedtree;
		funcobj.obj("typedtree",&typedtree);
		Obj ftype;
		typedtree.obj("type",&ftype);
		TType * func_type = getType(&ftype);
		const char * name = funcobj.string("name");
		func = Function::Create(cast<FunctionType>(func_type->type), Function::ExternalLinkage,name, C->m);
		BB = BasicBlock::Create(*C->ctx,"entry",func);
		B->SetInsertPoint(BB);
		
		Obj parameters;
		typedtree.obj("parameters",&parameters);
		int nP = parameters.size();
		Function::arg_iterator ai = func->arg_begin();
		for(int i = 1; i <= nP; i++) {
			Obj p;
			parameters.objAt(i,&p);
			AllocaInst * a = allocVar(&p);
			B->CreateStore(ai,a);
			++ai;
		}
		
		Obj body;
		typedtree.obj("body",&body);
		emitStmt(&body);
		
		C->m->dump();
		verifyFunction(*func);
		C->fpm->run(*func);
		C->m->dump();
		
		void * ptr = C->ee->getPointerToFunction(func);
		void ** data = (void**) lua_newuserdata(L,sizeof(void*));
		assert(ptr);
		*data = ptr;
		funcobj.setfield("fptr");
		
		//cleanup -- ensure we left the stack the way we started
		assert(lua_gettop(T->L) == ref_table);
		delete B;
	}
	Value * emitExp(Obj * exp) {
		exp->begin_match("kind");
		if(exp->matches("var")) {
			Obj def;
			exp->obj("definition",&def);
			Value * v = (Value*) def.ud("value");
			assert(v);
			return v;
		} else if(exp->matches("ltor")) {
			Obj e;
			exp->obj("expression",&e);
			Value * v = emitExp(&e);
			return B->CreateLoad(v);
		} else if(exp->matches("operator")) {
			exp->begin_match("operator");
			if(exp->matches("+")) {
				TType * t = typeOfValue(exp);
				Obj exps;
				exp->obj("operands",&exps);
				if(t->type->isFPOrFPVectorTy()) {
					Obj a,b;
					exps.objAt(1,&a);
					exps.objAt(2,&b);
					return B->CreateFAdd(emitExp(&a),emitExp(&b));
				} else {
					assert(!"NYI - integer +");
				}
			} else {
				assert(!"NYI - operator");
			}
		} else {
			assert(!"NYI - exp");
		}
	}
	void emitStmt(Obj * stmt) {
		stmt->begin_match("kind");
		if(stmt->matches("block")) {
			Obj stmts;
			stmt->obj("statements",&stmts);
			int N = stmts.size();
			for(int i = 1; i <= N; i++) {
				Obj s;
				stmts.objAt(i,&s);
				emitStmt(&s);
			}
		} else if(stmt->matches("defvar")) {
			std::vector<Value *> rhs;
			Obj inits;
			stmt->obj("initializers",&inits);
			int N = inits.size();
			for(int i = 1; i <= N; i++) {
				Obj init;
				inits.objAt(i,&init);
				rhs.push_back(emitExp(&init));
			}
			Obj vars;
			stmt->obj("variables",&vars);
			N = inits.size();
			for(int i = 1; i <= N; i++) {
				Obj v;
				vars.objAt(i,&v);
				AllocaInst * a = allocVar(&v);
				B->CreateStore(rhs[i-1],a);
			}
		} else if(stmt->matches("return")) {
			Obj exps;
			stmt->obj("expressions",&exps);
			if(exps.size() == 0) {
				B->CreateRetVoid();
			} else {
				Obj r;
				exps.objAt(1,&r);
				Value * v = emitExp(&r);
				B->CreateRet(v);
			}
		} else {
			assert(!"NYI - stmt");
		}
	}
};

static int terra_compile(lua_State * L) { //entry point into compiler from lua code
	terra_State * T = (terra_State*) lua_topointer(L,lua_upvalueindex(1));
	assert(T->L == L);
	
	{
		Compiler c;
		c.run(T);
	} //scope to ensure that c.func is destroyed before we pop the reference table off the stack
	
	lua_pop(T->L,1); //remove the reference table from stack
	return 0;
}