// Statements and Declarations

void check_statement(Checker *c, AstNode *node);

void check_statement_list(Checker *c, AstNode *node) {
	for (; node != NULL; node = node->next)
		check_statement(c, node);
}


// NOTE(bill): The last expression has to be a `return` statement
// TODO(bill): This is a mild hack and should be probably handled
// TODO(bill): Warn/err against code after `return` that it won't be executed
b32 check_is_terminating(Checker *c, AstNode *node);

b32 check_is_terminating_list(Checker *c, AstNode *node_list) {
	AstNode *end_of_list = node_list;
	for (; end_of_list != NULL; end_of_list = end_of_list->next) {
		if (end_of_list->next == NULL)
			break;
	}

	for (AstNode *node = end_of_list; node != NULL; node = node->prev) {
		if (node->kind == AstNode_EmptyStatement)
			continue;
		return check_is_terminating(c, node);
	}
	return false;
}

b32 check_is_terminating(Checker *c, AstNode *node) {
	switch (node->kind) {
	case AstNode_BlockStatement:
		return check_is_terminating_list(c, node->block_statement.list);

	case AstNode_ExpressionStatement:
		return check_is_terminating(c, node->expression_statement.expression);

	case AstNode_ReturnStatement:
		return true;

	case AstNode_IfStatement:
		if (node->if_statement.else_statement != NULL) {
			if (check_is_terminating(c, node->if_statement.body) &&
			    check_is_terminating(c, node->if_statement.else_statement))
			    return true;
		}
		break;

	case AstNode_ForStatement:
		if (node->for_statement.cond == NULL) {
			return true;
		}
		break;
	}

	return false;
}


b32 check_is_assignable_to(Checker *c, Operand *operand, Type *type) {
	if (operand->mode == Addressing_Invalid ||
	    type == &basic_types[Basic_Invalid]) {
		return true;
	}

	Type *s = operand->type;

	if (are_types_identical(s, type))
		return true;

	Type *sb = get_base_type(s);
	Type *tb = get_base_type(type);

	if (is_type_untyped(sb)) {
		switch (tb->kind) {
		case Type_Basic:
			if (operand->mode == Addressing_Constant)
				return check_value_is_expressible(c, operand->value, tb, NULL);
			if (sb->kind == Type_Basic)
				return sb->basic.kind == Basic_UntypedBool && is_type_boolean(tb);
			break;
		case Type_Pointer:
			return sb->basic.kind == Basic_UntypedPointer;
		}
	}

	if (are_types_identical(sb, tb) && (!is_type_named(sb) || !is_type_named(tb)))
		return true;

	if (is_type_pointer(sb) && is_type_rawptr(tb))
	    return true;

	if (is_type_rawptr(sb) && is_type_pointer(tb))
	    return true;

	if (sb->kind == Type_Array && tb->kind == Type_Array) {
		if (are_types_identical(sb->array.element, tb->array.element)) {
			return sb->array.count == tb->array.count;
		}
	}

	if (sb->kind == Type_Slice && tb->kind == Type_Slice) {
		if (are_types_identical(sb->slice.element, tb->slice.element)) {
			return true;
		}
	}

	return false;
}


// NOTE(bill): `content_name` is for debugging
// TODO(bill): Maybe allow assignment to tuples?
void check_assignment(Checker *c, Operand *operand, Type *type, String context_name) {
	check_not_tuple(c, operand);
	if (operand->mode == Addressing_Invalid)
		return;

	if (is_type_untyped(operand->type)) {
		Type *target_type = type;

		if (type == NULL)
			target_type = default_type(operand->type);
		convert_to_typed(c, operand, target_type);
		if (operand->mode == Addressing_Invalid)
			return;
	}

	if (type != NULL) {
		if (!check_is_assignable_to(c, operand, type)) {
			gbString type_string = type_to_string(type);
			gbString op_type_string = type_to_string(operand->type);
			defer (gb_string_free(type_string));
			defer (gb_string_free(op_type_string));

			// TODO(bill): is this a good enough error message?
			print_checker_error(c, ast_node_token(operand->expression),
			                    "Cannot assign value `%.*s` of type `%s` to `%s` in %.*s",
			                    LIT(ast_node_token(operand->expression).string),
			                    op_type_string,
			                    type_string,
			                    LIT(context_name));

			operand->mode = Addressing_Invalid;
		}
	}
}


Type *check_assign_variable(Checker *c, Operand *op_a, AstNode *lhs) {
	if (op_a->mode == Addressing_Invalid ||
	    op_a->type == &basic_types[Basic_Invalid]) {
		return NULL;
	}

	AstNode *node = unparen_expression(lhs);

	// NOTE(bill): Ignore assignments to `_`
	if (node->kind == AstNode_Identifier &&
	    are_strings_equal(node->identifier.token.string, make_string("_"))) {
    	add_entity_definition(c, node, NULL);
    	check_assignment(c, op_a, NULL, make_string("assignment to `_` identifier"));
    	if (op_a->mode == Addressing_Invalid)
    		return NULL;
    	return op_a->type;
    }

	Entity *e = NULL;
	b32 used = false;
	if (node->kind == AstNode_Identifier) {
		scope_lookup_parent_entity(c->curr_scope, node->identifier.token.string,
		                           NULL, &e);
		if (e != NULL && e->kind == Entity_Variable) {
			used = e->variable.used; // TODO(bill): Make backup just in case
		}
	}


	Operand op_b = {Addressing_Invalid};
	check_expression(c, &op_b, lhs);
	if (e) e->variable.used = used;

	if (op_b.mode == Addressing_Invalid ||
	    op_b.type == &basic_types[Basic_Invalid]) {
		return NULL;
	}

	switch (op_b.mode) {
	case Addressing_Variable:
		break;
	case Addressing_Invalid:
		return NULL;
	default: {
		if (op_b.expression->kind == AstNode_SelectorExpression) {
			// NOTE(bill): Extra error checks
			Operand op_c = {Addressing_Invalid};
			check_expression(c, &op_c, op_b.expression->selector_expression.operand);
		}

		gbString str = expression_to_string(op_b.expression);
		defer (gb_string_free(str));
		print_checker_error(c, ast_node_token(op_b.expression),
		                    "Cannot assign to `%s`", str);
	} break;
	}

	check_assignment(c, op_a, op_b.type, make_string("assignment"));
	if (op_a->mode == Addressing_Invalid)
		return NULL;

	return op_a->type;
}

// TODO(bill): Do I need to pass the *_count?
void check_assign_variables(Checker *c,
                            AstNode *lhs_list, isize lhs_count,
                            AstNode *rhs_list, isize rhs_count) {
	Operand operand = {Addressing_Invalid};
	AstNode *lhs = lhs_list, *rhs = rhs_list;
	for (;
	     lhs != NULL && rhs != NULL;
	     lhs = lhs->next, rhs = rhs->next) {
		check_multi_expression(c, &operand, rhs);
		if (operand.type->kind != Type_Tuple) {
			check_assign_variable(c, &operand, lhs);
		} else {
			auto *tuple = &operand.type->tuple;
			for (isize i = 0;
			     i < tuple->variable_count && lhs != NULL;
			     i++, lhs = lhs->next) {
				// TODO(bill): More error checking
				operand.type = tuple->variables[i]->type;
				check_assign_variable(c, &operand, lhs);
			}
			if (lhs == NULL)
				break;
		}
	}
}

// NOTE(bill): `content_name` is for debugging
Type *check_init_variable(Checker *c, Entity *e, Operand *operand, String context_name) {
	if (operand->mode == Addressing_Invalid ||
	    operand->type == &basic_types[Basic_Invalid] ||
	    e->type == &basic_types[Basic_Invalid]) {
		if (e->type == NULL)
			e->type = &basic_types[Basic_Invalid];
		return NULL;
	}

	if (e->type == NULL) {
		// NOTE(bill): Use the type of the operand
		Type *t = operand->type;
		if (is_type_untyped(t)) {
			if (t == &basic_types[Basic_Invalid]) {
				print_checker_error(c, e->token, "Use of untyped thing in %.*s", LIT(context_name));
				e->type = &basic_types[Basic_Invalid];
				return NULL;
			}
			t = default_type(t);
		}
		e->type = t;
	}

	check_assignment(c, operand, e->type, context_name);
	if (operand->mode == Addressing_Invalid)
		return NULL;

	return e->type;
}

void check_init_variables(Checker *c, Entity **lhs, isize lhs_count, AstNode *init_list, isize init_count, String context_name) {
	if ((lhs == NULL || lhs_count == 0) && init_count == 0)
		return;

	isize i = 0;
	AstNode *rhs = init_list;
	for (;
	     i < lhs_count && i < init_count && rhs != NULL;
	     i++, rhs = rhs->next) {
		Operand operand = {};
		check_multi_expression(c, &operand, rhs);
		if (operand.type->kind != Type_Tuple) {
			check_init_variable(c, lhs[i], &operand, context_name);
		} else {
			auto *tuple = &operand.type->tuple;
			for (isize j = 0;
			     j < tuple->variable_count && i < lhs_count && i < init_count;
			     j++, i++) {
				Type *type = tuple->variables[j]->type;
				operand.type = type;
				check_init_variable(c, lhs[i], &operand, context_name);
			}
		}
	}

	if (i < lhs_count && i < init_count) {
		if (lhs[i]->type == NULL)
			print_checker_error(c, lhs[i]->token, "Too few values on the right hand side of the declaration");
	} else if (rhs != NULL) {
		print_checker_error(c, ast_node_token(rhs), "Too many values on the right hand side of the declaration");
	}
}

void check_init_constant(Checker *c, Entity *e, Operand *operand) {
	if (operand->mode == Addressing_Invalid ||
	    operand->type == &basic_types[Basic_Invalid] ||
	    e->type == &basic_types[Basic_Invalid]) {
		if (e->type == NULL)
			e->type = &basic_types[Basic_Invalid];
		return;
	}

	if (operand->mode != Addressing_Constant) {
		// TODO(bill): better error
		print_checker_error(c, ast_node_token(operand->expression),
		                    "`%.*s` is not a constant", LIT(ast_node_token(operand->expression).string));
		if (e->type == NULL)
			e->type = &basic_types[Basic_Invalid];
		return;
	}
	if (!is_type_constant_type(operand->type)) {
		// NOTE(bill): no need to free string as it's panicking
		GB_PANIC("Compiler error: Type `%s` not constant!!!", type_to_string(operand->type));
	}

	if (e->type == NULL) // NOTE(bill): type inference
		e->type = operand->type;

	check_assignment(c, operand, e->type, make_string("constant declaration"));
	if (operand->mode == Addressing_Invalid)
		return;

	e->constant.value = operand->value;
}


void check_constant_declaration(Checker *c, Entity *e, AstNode *type_expr, AstNode *init_expr) {
	GB_ASSERT(e->type == NULL);

	if (e->variable.visited) {
		e->type = &basic_types[Basic_Invalid];
		return;
	}
	e->variable.visited = true;

	if (type_expr) {
		Type *t = check_type(c, type_expr);
		if (!is_type_constant_type(t)) {
			gbString str = type_to_string(t);
			defer (gb_string_free(str));
			print_checker_error(c, ast_node_token(type_expr),
			                    "Invalid constant type `%s`", str);
			e->type = &basic_types[Basic_Invalid];
			return;
		}
		e->type = t;
	}

	Operand operand = {Addressing_Invalid};
	if (init_expr)
		check_expression(c, &operand, init_expr);
	check_init_constant(c, e, &operand);
}

void check_type_declaration(Checker *c, Entity *e, AstNode *type_expr, Type *named_type) {
	GB_ASSERT(e->type == NULL);
	Type *named = make_type_named(c->allocator, e->token.string, NULL, e);
	named->named.type_name = e;
	set_base_type(named_type, named);
	e->type = named;

	check_type(c, type_expr, named);

	set_base_type(named, get_base_type(get_base_type(named)));
}


void check_procedure_body(Checker *c, Token token, DeclarationInfo *decl, Type *type, AstNode *body) {
	GB_ASSERT(body->kind == AstNode_BlockStatement);
	Scope *origin_curr_scope = c->curr_scope;
	c->curr_scope = decl->scope;
	push_procedure(c, type);
	check_statement_list(c, body->block_statement.list);
	if (decl->type_expr != NULL &&
	    decl->type_expr->procedure_type.result_count > 0) {
		if (!check_is_terminating(c, body)) {
			print_checker_error(c, body->block_statement.close, "Missing return statement at the end of the procedure");
		}
	}
	pop_procedure(c);

	c->curr_scope = origin_curr_scope;
}

void check_procedure_declaration(Checker *c, Entity *e, DeclarationInfo *d, b32 check_body_later) {
	GB_ASSERT(e->type == NULL);

	Type *proc_type = make_type_procedure(c->allocator, e->parent, NULL, 0, NULL, 0);
	e->type = proc_type;
	auto *pd = &d->proc_decl->procedure_declaration;

#if 1
	Scope *origin_curr_scope = c->curr_scope;
	c->curr_scope = c->file_scope;
	check_open_scope(c, pd->procedure_type);
#endif
	check_procedure_type(c, proc_type, pd->procedure_type);
	b32 is_foreign   = false;
	b32 is_inline    = false;
	b32 is_no_inline = false;
	for (AstNode *tag = pd->tag_list; tag != NULL; tag = tag->next) {
		GB_ASSERT(tag->kind == AstNode_TagExpression);

		String tag_name = tag->tag_expression.name.string;
		if (are_strings_equal(tag_name, make_string("foreign"))) {
			is_foreign = true;
		} else if (are_strings_equal(tag_name, make_string("inline"))) {
			is_inline = true;
		} else if (are_strings_equal(tag_name, make_string("no_inline"))) {
			is_no_inline = true;
		} else {
			print_checker_error(c, ast_node_token(tag), "Unknown procedure tag");
		}
		// TODO(bill): Other tags
	}

	if (is_inline && is_no_inline) {
		print_checker_error(c, ast_node_token(pd->tag_list),
		                    "You cannot apply both `inline` and `no_inline` to a procedure");
	}

	if (pd->body != NULL) {
		if (is_foreign) {
			print_checker_error(c, ast_node_token(pd->body),
			                    "A procedure tagged as `#foreign` cannot have a body");
		}

		d->scope = c->curr_scope;

		GB_ASSERT(pd->body->kind == AstNode_BlockStatement);
		if (check_body_later) {
			check_procedure_later(c, e->token, d, proc_type, pd->body);
		} else {
			check_procedure_body(c, e->token, d, proc_type, pd->body);
		}
	}

#if 1
	check_close_scope(c);
	c->curr_scope = origin_curr_scope;
#endif

}

void check_variable_declaration(Checker *c, Entity *e, Entity **entities, isize entity_count, AstNode *type_expr, AstNode *init_expr) {
	GB_ASSERT(e->type == NULL);
	GB_ASSERT(e->kind == Entity_Variable);

	if (e->variable.visited) {
		e->type = &basic_types[Basic_Invalid];
		return;
	}
	e->variable.visited = true;

	if (type_expr != NULL)
		e->type = check_type(c, type_expr, NULL);

	if (init_expr == NULL) {
		if (type_expr == NULL)
			e->type = &basic_types[Basic_Invalid];
		return;
	}

	if (entities == NULL || entity_count == 1) {
		GB_ASSERT(entities == NULL || entities[0] == e);
		Operand operand = {};
		check_expression(c, &operand, init_expr);
		check_init_variable(c, e, &operand, make_string("variable declaration"));
	}

	if (type_expr != NULL) {
		for (isize i = 0; i < entity_count; i++)
			entities[i]->type = e->type;
	}

	check_init_variables(c, entities, entity_count, init_expr, 1, make_string("variable declaration"));
}



void check_entity_declaration(Checker *c, Entity *e, Type *named_type) {
	if (e->type != NULL)
		return;

	DeclarationInfo **found = map_get(&c->entities, hash_pointer(e));
	if (found == NULL) {
		GB_PANIC("Compiler error: This entity should be declared!");
	}
	DeclarationInfo *d = *found;

	switch (e->kind) {
	case Entity_Constant:
		c->decl = d;
		check_constant_declaration(c, e, d->type_expr, d->init_expr);
		break;
	case Entity_Variable:
		c->decl = d;
		check_variable_declaration(c, e, d->entities, d->entity_count, d->type_expr, d->init_expr);
		break;
	case Entity_TypeName:
		check_type_declaration(c, e, d->type_expr, named_type);
		break;
	case Entity_Procedure:
		check_procedure_declaration(c, e, d, true);
		break;
	}

}




void check_statement(Checker *c, AstNode *node) {
	switch (node->kind) {
	case AstNode_EmptyStatement: break;
	case AstNode_BadStatement:   break;
	case AstNode_BadDeclaration: break;

	case AstNode_ExpressionStatement: {
		Operand operand = {Addressing_Invalid};
		ExpressionKind kind = check_expression_base(c, &operand, node->expression_statement.expression);
		switch (operand.mode) {
		case Addressing_Type:
			print_checker_error(c, ast_node_token(node), "Is not an expression");
			break;
		default:
			if (kind == Expression_Statement)
				return;
			print_checker_error(c, ast_node_token(node), "Expression is not used");
			break;
		}
	} break;

	case AstNode_TagStatement:
		// TODO(bill): Tag Statements
		print_checker_error(c, ast_node_token(node), "Tag statements are not supported yet");
		check_statement(c, node->tag_statement.statement);
		break;

	case AstNode_IncDecStatement: {
		Token op = {};
		auto *s = &node->inc_dec_statement;
		op = s->op;
		switch (s->op.kind) {
		case Token_Increment:
			op.kind = Token_Add;
			op.string.len = 1;
			break;
		case Token_Decrement:
			op.kind = Token_Sub;
			op.string.len = 1;
			break;
		default:
			print_checker_error(c, s->op, "Unknown inc/dec operation %.*s", LIT(s->op.string));
			return;
		}

		Operand operand = {Addressing_Invalid};
		check_expression(c, &operand, s->expression);
		if (operand.mode == Addressing_Invalid)
			return;
		if (!is_type_numeric(operand.type)) {
			print_checker_error(c, s->op, "Non numeric type");
			return;
		}

		AstNode basic_lit = {AstNode_BasicLiteral};
		basic_lit.basic_literal = s->op;
		basic_lit.basic_literal.kind = Token_Integer;
		basic_lit.basic_literal.string = make_string("1");
		AstNode be = {AstNode_BinaryExpression};
		be.binary_expression.op = op;
		be.binary_expression.left = s->expression;;
		be.binary_expression.right = &basic_lit;
		check_binary_expression(c, &operand, &be);

	} break;

	case AstNode_AssignStatement:
		switch (node->assign_statement.op.kind) {
		case Token_Eq:
			if (node->assign_statement.lhs_count == 0) {
				print_checker_error(c, node->assign_statement.op, "Missing lhs in assignment statement");
				return;
			}
			check_assign_variables(c,
			                       node->assign_statement.lhs_list, node->assign_statement.lhs_count,
			                       node->assign_statement.rhs_list, node->assign_statement.rhs_count);
			break;

		default: {
			Token op = node->assign_statement.op;
			if (node->assign_statement.lhs_count != 1 ||
			    node->assign_statement.rhs_count != 1) {
				print_checker_error(c, op,
				                    "assignment operation `%.*s` requires single-valued expressions", LIT(op.string));
				return;
			}
			// TODO(bill): Check if valid assignment operator
			Operand operand = {Addressing_Invalid};
			AstNode be = {AstNode_BinaryExpression};
			be.binary_expression.op    = op;
			 // NOTE(bill): Only use the first one will be used
			be.binary_expression.left  = node->assign_statement.lhs_list;
			be.binary_expression.right = node->assign_statement.rhs_list;

			check_binary_expression(c, &operand, &be);
			if (operand.mode == Addressing_Invalid)
				return;
			// NOTE(bill): Only use the first one will be used
			check_assign_variable(c, &operand, node->assign_statement.lhs_list);
		} break;
		}
		break;

	case AstNode_BlockStatement:
		check_open_scope(c, node);
		check_statement_list(c, node->block_statement.list);
		check_close_scope(c);
		break;

	case AstNode_IfStatement: {
		Operand operand = {Addressing_Invalid};
		check_expression(c, &operand, node->if_statement.cond);
		if (operand.mode != Addressing_Invalid &&
		    !is_type_boolean(operand.type)) {
			print_checker_error(c, ast_node_token(node->if_statement.cond),
			                    "Non-boolean condition in `if` statement");
		}
		check_statement(c, node->if_statement.body);

		if (node->if_statement.else_statement) {
			switch (node->if_statement.else_statement->kind) {
			case AstNode_IfStatement:
			case AstNode_BlockStatement:
				check_statement(c, node->if_statement.else_statement);
				break;
			default:
				print_checker_error(c, ast_node_token(node->if_statement.else_statement),
				                    "Invalid `else` statement in `if` statement");
				break;
			}
		}
	} break;

	case AstNode_ReturnStatement: {
		auto *rs = &node->return_statement;
		GB_ASSERT(gb_array_count(c->procedure_stack) > 0);

		if (c->in_defer) {
			print_checker_error(c, rs->token, "You cannot `return` within a defer statement");
			// TODO(bill): Should I break here?
			break;
		}

		Type *proc_type = c->procedure_stack[gb_array_count(c->procedure_stack)-1];
		isize result_count = 0;
		if (proc_type->procedure.results)
			result_count = proc_type->procedure.results->tuple.variable_count;
		if (result_count != rs->result_count) {
			print_checker_error(c, rs->token, "Expected %td return %s, got %td",
			                    result_count,
			                    (result_count != 1 ? "values" : "value"),
			                    rs->result_count);
		} else if (result_count > 0) {
			auto *tuple = &proc_type->procedure.results->tuple;
			check_init_variables(c, tuple->variables, tuple->variable_count,
			                     rs->results, rs->result_count, make_string("return statement"));
		}
	} break;

	case AstNode_ForStatement: {
		check_open_scope(c, node);
		defer (check_close_scope(c));

		check_statement(c, node->for_statement.init);
		if (node->for_statement.cond) {
			Operand operand = {Addressing_Invalid};
			check_expression(c, &operand, node->for_statement.cond);
			if (operand.mode != Addressing_Invalid &&
			    !is_type_boolean(operand.type)) {
				print_checker_error(c, ast_node_token(node->for_statement.cond),
				                    "Non-boolean condition in `for` statement");
			}
		}
		check_statement(c, node->for_statement.end);
		check_statement(c, node->for_statement.body);
	} break;

	case AstNode_DeferStatement: {
		auto *ds = &node->defer_statement;
		if (is_ast_node_declaration(ds->statement)) {
			print_checker_error(c, ds->token, "You cannot defer a declaration");
		} else {
			b32 out_in_defer = c->in_defer;
			c->in_defer = true;
			check_statement(c, ds->statement);
			c->in_defer = out_in_defer;
		}
	} break;


// Declarations
	case AstNode_VariableDeclaration: {
		auto *vd = &node->variable_declaration;
		isize entity_count = vd->name_list_count;
		isize entity_index = 0;
		Entity **entities = gb_alloc_array(c->allocator, Entity *, entity_count);
		switch (vd->kind) {
		case Declaration_Mutable: {
			Entity **new_entities = gb_alloc_array(c->allocator, Entity *, entity_count);
			isize new_entity_count = 0;

			for (AstNode *name = vd->name_list; name != NULL; name = name->next) {
				Entity *entity = NULL;
				Token token = name->identifier.token;
				if (name->kind == AstNode_Identifier) {
					String str = token.string;
					Entity *found = NULL;
					// NOTE(bill): Ignore assignments to `_`
					b32 can_be_ignored = are_strings_equal(str, make_string("_"));
					if (!can_be_ignored) {
						found = current_scope_lookup_entity(c->curr_scope, str);
					}
					if (found == NULL) {
						entity = make_entity_variable(c->allocator, c->curr_scope, token, NULL);
						if (!can_be_ignored) {
							new_entities[new_entity_count++] = entity;
						}
						add_entity_definition(c, name, entity);
					} else {
						entity = found;
					}
				} else {
					print_checker_error(c, token, "A variable declaration must be an identifier");
				}
				if (entity == NULL)
					entity = make_entity_dummy_variable(c->allocator, c->file_scope, token);
				entities[entity_index++] = entity;
			}

			Type *init_type = NULL;
			if (vd->type_expression) {
				init_type = check_type(c, vd->type_expression, NULL);
				if (init_type == NULL)
					init_type = &basic_types[Basic_Invalid];
			}

			for (isize i = 0; i < entity_count; i++) {
				Entity *e = entities[i];
				GB_ASSERT(e != NULL);
				if (e->variable.visited) {
					e->type = &basic_types[Basic_Invalid];
					continue;
				}
				e->variable.visited = true;

				if (e->type == NULL)
					e->type = init_type;
			}


			check_init_variables(c, entities, entity_count, vd->value_list, vd->value_list_count, make_string("variable declaration"));

			AstNode *name = vd->name_list;
			for (isize i = 0; i < new_entity_count; i++, name = name->next) {
				add_entity(c, c->curr_scope, name, new_entities[i]);
			}

		} break;

		case Declaration_Immutable: {
			for (AstNode *name = vd->name_list, *value = vd->value_list;
			     name != NULL && value != NULL;
			     name = name->next, value = value->next) {
				GB_ASSERT(name->kind == AstNode_Identifier);
				Value v = {Value_Invalid};
				Entity *e = make_entity_constant(c->allocator, c->curr_scope, name->identifier.token, NULL, v);
				entities[entity_index++] = e;
				check_constant_declaration(c, e, vd->type_expression, value);
			}

			isize lhs_count = vd->name_list_count;
			isize rhs_count = vd->value_list_count;

			// TODO(bill): Better error messages or is this good enough?
			if (rhs_count == 0 && vd->type_expression == NULL) {
				print_checker_error(c, ast_node_token(node), "Missing type or initial expression");
			} else if (lhs_count < rhs_count) {
				print_checker_error(c, ast_node_token(node), "Extra initial expression");
			}

			AstNode *name = vd->name_list;
			for (isize i = 0; i < entity_count; i++, name = name->next) {
				add_entity(c, c->curr_scope, name, entities[i]);
			}
		} break;

		default:
			print_checker_error(c, ast_node_token(node), "Unknown variable declaration kind. Probably an invalid AST.");
			return;
		}
	} break;

	case AstNode_ProcedureDeclaration: {
		auto *pd = &node->procedure_declaration;
		Entity *e = make_entity_procedure(c->allocator, c->curr_scope, pd->name->identifier.token, NULL);
		add_entity(c, c->curr_scope, pd->name, e);

		DeclarationInfo *decl = make_declaration_info(gb_heap_allocator(), e->parent);
		decl->proc_decl = node;
		check_procedure_declaration(c, e, decl, false);
		destroy_declaration_info(decl);
	} break;

	case AstNode_TypeDeclaration: {
		auto *td = &node->type_declaration;
		AstNode *name = td->name;
		Entity *e = make_entity_type_name(c->allocator, c->curr_scope, name->identifier.token, NULL);
		add_entity(c, c->curr_scope, name, e);
		check_type_declaration(c, e, td->type_expression, NULL);
	} break;
	}
}
