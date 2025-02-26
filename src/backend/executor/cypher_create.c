/*
 * Licensed to the Apache Software Foundation (ASF) under one
 * or more contributor license agreements.  See the NOTICE file
 * distributed with this work for additional information
 * regarding copyright ownership.  The ASF licenses this file
 * to you under the Apache License, Version 2.0 (the
 * "License"); you may not use this file except in compliance
 * with the License.  You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing,
 * software distributed under the License is distributed on an
 * "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
 * KIND, either express or implied.  See the License for the
 * specific language governing permissions and limitations
 * under the License.
 */

#include "postgres.h"

#include "access/htup_details.h"
#include "access/xact.h"
#include "executor/tuptable.h"
#include "nodes/execnodes.h"
#include "nodes/extensible.h"
#include "nodes/nodes.h"
#include "nodes/plannodes.h"
#include "parser/parse_relation.h"
#include "rewrite/rewriteHandler.h"
#include "utils/rel.h"
#include "utils/tqual.h"

#include "catalog/ag_label.h"
#include "executor/cypher_executor.h"
#include "executor/cypher_utils.h"
#include "nodes/cypher_nodes.h"
#include "utils/agtype.h"
#include "utils/ag_cache.h"
#include "utils/graphid.h"

static void begin_cypher_create(CustomScanState *node, EState *estate,
                                int eflags);
static TupleTableSlot *exec_cypher_create(CustomScanState *node);
static void end_cypher_create(CustomScanState *node);
static void rescan_cypher_create(CustomScanState *node);

static void create_edge(cypher_create_custom_scan_state *css,
                        cypher_target_node *node, Datum prev_vertex_id,
                        ListCell *next);

static Datum create_vertex(cypher_create_custom_scan_state *css,
                           cypher_target_node *node, ListCell *next);
static HeapTuple insert_entity_tuple(ResultRelInfo *resultRelInfo,
                                TupleTableSlot *elemTupleSlot, EState *estate);
static void process_pattern(cypher_create_custom_scan_state *css);
static bool entity_exists(EState *estate, Oid graph_oid, graphid id);

const CustomExecMethods cypher_create_exec_methods = {CREATE_SCAN_STATE_NAME,
                                                      begin_cypher_create,
                                                      exec_cypher_create,
                                                      end_cypher_create,
                                                      rescan_cypher_create,
                                                      NULL,
                                                      NULL,
                                                      NULL,
                                                      NULL,
                                                      NULL,
                                                      NULL,
                                                      NULL,
                                                      NULL};

static void begin_cypher_create(CustomScanState *node, EState *estate,
                                int eflags)
{
    cypher_create_custom_scan_state *css =
        (cypher_create_custom_scan_state *)node;
    ListCell *lc;
    Plan *subplan;

    Assert(list_length(css->cs->custom_plans) == 1);

    subplan = linitial(css->cs->custom_plans);
    node->ss.ps.lefttree = ExecInitNode(subplan, estate, eflags);

    ExecAssignExprContext(estate, &node->ss.ps);

    ExecInitScanTupleSlot(estate, &node->ss,
                          ExecGetResultType(node->ss.ps.lefttree));

    if (!CYPHER_CLAUSE_IS_TERMINAL(css->flags))
    {
        TupleDesc tupdesc = node->ss.ss_ScanTupleSlot->tts_tupleDescriptor;

        ExecAssignProjectionInfo(&node->ss.ps, tupdesc);
    }

    foreach (lc, css->pattern)
    {
        cypher_create_path *path = lfirst(lc);
        ListCell *lc2;
        foreach (lc2, path->target_nodes)
        {
            cypher_target_node *cypher_node =
                (cypher_target_node *)lfirst(lc2);
            Relation rel;

            if (!CYPHER_TARGET_NODE_INSERT_ENTITY(cypher_node->flags))
                continue;

            // Open relation and aquire a row exclusive lock.
            rel = heap_open(cypher_node->relid, RowExclusiveLock);

            // Initialize resultRelInfo for the vertex
            cypher_node->resultRelInfo = makeNode(ResultRelInfo);
            InitResultRelInfo(cypher_node->resultRelInfo, rel,
                              list_length(estate->es_range_table), NULL,
                              estate->es_instrument);

            // Open all indexes for the relation
            ExecOpenIndices(cypher_node->resultRelInfo, false);

            // Setup the relation's tuple slot
            cypher_node->elemTupleSlot = ExecInitExtraTupleSlot(
                estate,
                RelationGetDescr(cypher_node->resultRelInfo->ri_RelationDesc));

            if (cypher_node->id_expr != NULL)
            {
                cypher_node->id_expr_state =
                    ExecInitExpr(cypher_node->id_expr, (PlanState *)node);
            }
        }
    }

    /*
     * Postgres does not assign the es_output_cid in queries that do
     * not write to disk, ie: SELECT commands. We need the command id
     * for our clauses, and we may need to initialize it. We cannot use
     * GetCurrentCommandId because there may be other cypher clauses
     * that have modified the command id.
     */
    if (estate->es_output_cid == 0)
        estate->es_output_cid = estate->es_snapshot->curcid;

    CommandCounterIncrement();
    Increment_Estate_CommandId(estate);
}

/*
 * CREATE the vertices and edges for a CREATE clause pattern.
 */
static void process_pattern(cypher_create_custom_scan_state *css)
{
    ListCell *lc2;

    css->tuple_info = NIL;

    foreach (lc2, css->pattern)
    {
        cypher_create_path *path = lfirst(lc2);

        ListCell *lc = list_head(path->target_nodes);

        /*
         * Create the first vertex. The create_vertex function will
         * create the rest of the path, if necessary.
         */
        create_vertex(css, lfirst(lc), lnext(lc));

        /*
         * If this path is a variable, take the list that was accumulated
         * in the vertex/edge creation, create a path datum, and add to the
         * scantuple slot.
         */
        if (path->path_attr_num != InvalidAttrNumber)
        {
            TupleTableSlot *scantuple;
            PlanState *ps;
            Datum result;

            ps = css->css.ss.ps.lefttree;
            scantuple = ps->ps_ExprContext->ecxt_scantuple;

            result = make_path(css->path_values);

            scantuple->tts_values[path->path_attr_num - 1] = result;
            scantuple->tts_isnull[path->path_attr_num - 1] = false;
        }

        css->path_values = NIL;
    }
}

static TupleTableSlot *exec_cypher_create(CustomScanState *node)
{
    cypher_create_custom_scan_state *css =
        (cypher_create_custom_scan_state *)node;
    EState *estate = css->css.ss.ps.state;
    ExprContext *econtext = css->css.ss.ps.ps_ExprContext;
    TupleTableSlot *slot;

    if (CYPHER_CLAUSE_IS_TERMINAL(css->flags))
    {
        /*
         * If the CREATE clause was the final cypher clause written
         * then we aren't returning anything from this result node.
         * So the exec_cypher_create function will only be called once.
         * Therefore we will process all tuples from the subtree at once.
         */
        while(true)
        {
            //Process the subtree first
            Decrement_Estate_CommandId(estate)
            slot = ExecProcNode(node->ss.ps.lefttree);
            Increment_Estate_CommandId(estate)

            if (TupIsNull(slot))
                break;

            // setup the scantuple that the process_pattern needs
            econtext->ecxt_scantuple =
                node->ss.ps.lefttree->ps_ProjInfo->pi_exprContext->ecxt_scantuple;

            css->tuple_info = NIL;

            process_pattern(css);
        }

        return NULL;
    }
    else
    {
        //Process the subtree first
        Decrement_Estate_CommandId(estate)
        slot = ExecProcNode(node->ss.ps.lefttree);
        Increment_Estate_CommandId(estate)

        if (TupIsNull(slot))
            return NULL;

        // setup the scantuple that the process_delete_list needs
        econtext->ecxt_scantuple =
            node->ss.ps.lefttree->ps_ProjInfo->pi_exprContext->ecxt_scantuple;

        css->tuple_info = NIL;

        process_pattern(css);

        econtext->ecxt_scantuple =
            ExecProject(node->ss.ps.lefttree->ps_ProjInfo);

        return ExecProject(node->ss.ps.ps_ProjInfo);
    }
}

static void end_cypher_create(CustomScanState *node)
{
    cypher_create_custom_scan_state *css =
        (cypher_create_custom_scan_state *)node;
    ListCell *lc;

    ExecEndNode(node->ss.ps.lefttree);

    foreach (lc, css->pattern)
    {
        cypher_create_path *path = lfirst(lc);
        ListCell *lc2;
        foreach (lc2, path->target_nodes)
        {
            cypher_target_node *cypher_node =
                (cypher_target_node *)lfirst(lc2);

            if (!CYPHER_TARGET_NODE_INSERT_ENTITY(cypher_node->flags))
                continue;

            // close all indices for the node
            ExecCloseIndices(cypher_node->resultRelInfo);

            // close the relation itself
            heap_close(cypher_node->resultRelInfo->ri_RelationDesc,
                       RowExclusiveLock);
        }
    }
}

static void rescan_cypher_create(CustomScanState *node)
{
    ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                    errmsg("cypher create clause cannot be rescaned"),
                    errhint("its unsafe to use joins in a query with a Cypher CREATE clause")));
}

Node *create_cypher_create_plan_state(CustomScan *cscan)
{
    cypher_create_custom_scan_state *cypher_css =
        palloc0(sizeof(cypher_create_custom_scan_state));
    cypher_create_target_nodes *target_nodes;
    char *serialized_data;
    Const *c;

    cypher_css->cs = cscan;

    // get the serialized data structure from the Const and deserialize it.
    c = linitial(cscan->custom_private);
    serialized_data = (char *)c->constvalue;
    target_nodes = stringToNode(serialized_data);

    Assert(is_ag_node(target_nodes, cypher_create_target_nodes));

    cypher_css->path_values = NIL;
    cypher_css->pattern = target_nodes->paths;
    cypher_css->tuple_info = NIL;
    cypher_css->flags = target_nodes->flags;
    cypher_css->graph_oid = target_nodes->graph_oid;

    cypher_css->css.ss.ps.type = T_CustomScanState;
    cypher_css->css.methods = &cypher_create_exec_methods;

    return (Node *)cypher_css;
}

/*
 * Create the edge entity.
 */
static void create_edge(cypher_create_custom_scan_state *css,
                        cypher_target_node *node, Datum prev_vertex_id,
                        ListCell *next)
{
    bool isNull;
    EState *estate = css->css.ss.ps.state;
    ExprContext *econtext = css->css.ss.ps.ps_ExprContext;
    ResultRelInfo *resultRelInfo = node->resultRelInfo;
    TupleTableSlot *elemTupleSlot = node->elemTupleSlot;
    TupleTableSlot *scanTupleSlot = econtext->ecxt_scantuple;
    Datum id;
    Datum start_id, end_id, next_vertex_id;
    List *prev_path = css->path_values;
    HeapTuple tuple;

    Assert(node->type == LABEL_KIND_EDGE);
    Assert(lfirst(next) != NULL);

    /*
     * Create the next vertex before creating the edge. We need the
     * next vertex's id.
     */
    css->path_values = NIL;
    next_vertex_id = create_vertex(css, lfirst(next), lnext(next));

    /*
     * Set the start and end vertex ids
     */
    if (node->dir == CYPHER_REL_DIR_RIGHT)
    {
        // create pattern (prev_vertex)-[edge]->(next_vertex)
        start_id = prev_vertex_id;
        end_id = next_vertex_id;
    }
    else if (node->dir == CYPHER_REL_DIR_LEFT)
    {
        // create pattern (prev_vertex)<-[edge]-(next_vertex)
        start_id = next_vertex_id;
        end_id = prev_vertex_id;
    }
    else
    {
        ereport(ERROR,
                (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                 errmsg("edge direction must be specified in a CREATE clause")));
    }

    /*
     * Set estate's result relation to the vertex's result
     * relation.
     *
     * Note: This obliterates what was their previously
     */
    estate->es_result_relation_info = resultRelInfo;

    ExecClearTuple(elemTupleSlot);

    // Graph Id for the edge
    id = ExecEvalExpr(node->id_expr_state, econtext, &isNull);
    elemTupleSlot->tts_values[edge_tuple_id] = id;
    elemTupleSlot->tts_isnull[edge_tuple_id] = isNull;

    // Graph id for the starting vertex
    elemTupleSlot->tts_values[edge_tuple_start_id] = start_id;
    elemTupleSlot->tts_isnull[edge_tuple_start_id] = false;

    // Graph id for the ending vertex
    elemTupleSlot->tts_values[edge_tuple_end_id] = end_id;
    elemTupleSlot->tts_isnull[edge_tuple_end_id] = false;

    // Edge's properties map
    elemTupleSlot->tts_values[edge_tuple_properties] =
        scanTupleSlot->tts_values[node->prop_attr_num];
    elemTupleSlot->tts_isnull[edge_tuple_properties] =
        scanTupleSlot->tts_isnull[node->prop_attr_num];

    // Insert the new edge
    tuple = insert_entity_tuple(resultRelInfo, elemTupleSlot, estate);

    if (node->variable_name != NULL)
        css->tuple_info = add_tuple_info(css->tuple_info, tuple, node->variable_name);

    /*
     * When the edge is used by clauses higher in the execution tree
     * we need to create an edge datum. When the edge is a variable,
     * add to the scantuple slot. When the edge is part of a path
     * variable, add to the list.
     */
    if (CYPHER_TARGET_NODE_OUTPUT(node->flags))
    {
        PlanState *ps = css->css.ss.ps.lefttree;
        TupleTableSlot *scantuple = ps->ps_ExprContext->ecxt_scantuple;
        Datum result;

        result = make_edge(
            id, start_id, end_id, CStringGetDatum(node->label_name),
            PointerGetDatum(scanTupleSlot->tts_values[node->prop_attr_num]));

        if (CYPHER_TARGET_NODE_IN_PATH(node->flags))
        {
            prev_path = lappend(prev_path, DatumGetPointer(result));
            css->path_values = list_concat(prev_path, css->path_values);
        }
        if (CYPHER_TARGET_NODE_IS_VARIABLE(node->flags))
        {
            scantuple->tts_values[node->tuple_position - 1] = result;
            scantuple->tts_isnull[node->tuple_position - 1] = false;
        }
    }
}

/*
 * Creates the vertex entity, returns the vertex's id in case the caller is
 * the create_edge function.
 */
static Datum create_vertex(cypher_create_custom_scan_state *css,
                           cypher_target_node *node, ListCell *next)
{
    bool isNull;
    Datum id;
    EState *estate = css->css.ss.ps.state;
    ExprContext *econtext = css->css.ss.ps.ps_ExprContext;
    ResultRelInfo *resultRelInfo = node->resultRelInfo;
    TupleTableSlot *elemTupleSlot = node->elemTupleSlot;
    TupleTableSlot *scanTupleSlot = econtext->ecxt_scantuple;

    Assert(node->type == LABEL_KIND_VERTEX);

    /*
     * Vertices in a path might already exists. If they do get the id
     * to pass to the edges before and after it. Otherwise, insert the
     * new vertex into it's table and then pass the id along.
     */
    if (CYPHER_TARGET_NODE_INSERT_ENTITY(node->flags))
    {
        HeapTuple tuple;

        /*
         * Set estate's result relation to the vertex's result
         * relation.
         *
         * Note: This obliterates what was their previously
         */
        estate->es_result_relation_info = resultRelInfo;

        ExecClearTuple(elemTupleSlot);

        // get the next graphid for this vertex.
        id = ExecEvalExpr(node->id_expr_state, econtext, &isNull);
        elemTupleSlot->tts_values[vertex_tuple_id] = id;
        elemTupleSlot->tts_isnull[vertex_tuple_id] = isNull;

        // get the properties for this vertex
        elemTupleSlot->tts_values[vertex_tuple_properties] =
            scanTupleSlot->tts_values[node->prop_attr_num];
        elemTupleSlot->tts_isnull[vertex_tuple_properties] =
            scanTupleSlot->tts_isnull[node->prop_attr_num];

        // Insert the new vertex
        tuple = insert_entity_tuple(resultRelInfo, elemTupleSlot, estate);

        /*
         * If this vertex is a variable store the newly created tuple in
         * the CustomScanState. This will tell future clauses what the
         * tuple is for this variable, which is needed if the query wants
         * to update this tuple.
         */
        if (node->variable_name != NULL)
        {
            clause_tuple_information *tuple_info;

            tuple_info = palloc(sizeof(clause_tuple_information));

            tuple_info->tuple = tuple;
            tuple_info->name = node->variable_name;

            css->tuple_info = lappend(css->tuple_info, tuple_info);
        }

        /*
         * When the vertex is used by clauses higher in the execution tree
         * we need to create a vertex datum. When the vertex is a variable,
         * add to the scantuple slot. When the vertex is part of a path
         * variable, add to the list.
         */
        if (CYPHER_TARGET_NODE_OUTPUT(node->flags))
        {
            TupleTableSlot *scantuple;
            PlanState *ps;
            Datum result;

            ps = css->css.ss.ps.lefttree;
            scantuple = ps->ps_ExprContext->ecxt_scantuple;

            // make the vertex agtype
            result = make_vertex(
                id, CStringGetDatum(node->label_name),
                PointerGetDatum(scanTupleSlot->tts_values[node->prop_attr_num]));

            // append to the path list
            if (CYPHER_TARGET_NODE_IN_PATH(node->flags))
            {
                css->path_values = lappend(css->path_values, DatumGetPointer(result));
            }

            /*
             * Put the vertex in the correct spot in the scantuple, so parent execution
             * nodes can reference the newly created variable.
             */
            if (CYPHER_TARGET_NODE_IS_VARIABLE(node->flags))
            {
                scantuple->tts_values[node->tuple_position - 1] = result;
                scantuple->tts_isnull[node->tuple_position - 1] = false;
            }
        }
    }
    else
    {
        agtype *a;
        agtype_value *v;
        agtype_value *id_value;
        TupleTableSlot *scantuple;
        PlanState *ps;

        ps = css->css.ss.ps.lefttree;
        scantuple = ps->ps_ExprContext->ecxt_scantuple;

        // get the vertex agtype in the scanTupleSlot
        a = DATUM_GET_AGTYPE_P(scantuple->tts_values[node->tuple_position - 1]);

        // Convert to an agtype value
        v = get_ith_agtype_value_from_container(&a->root, 0);

        if (v->type != AGTV_VERTEX)
            ereport(ERROR,
                    (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                     errmsg("agtype must resolve to a vertex")));

        // extract the id agtype field
        id_value = get_agtype_value_object_value(v, "id");

        // extract the graphid and cast to a Datum
        id = GRAPHID_GET_DATUM(id_value->val.int_value);

        /*
         * Its possible the variable has already been deleted. There are two ways
         * this can happen. One is the query explicitly deleted the variable, the
         * is_deleted flag will catch that. However, it is possible the user deleted
         * the vertex using another variable name. We need to scan the table to find
         * the vertex's current status relative to this CREATE clause. If the variable
         * was initially created in this clause, we can skip this check, because the
         * transaction system guarantees that nothing can happen to that tuple, as
         * far as we are concerned with at this time.
         */
        if (!SAFE_TO_SKIP_EXISTENCE_CHECK(node->flags))
        {
            bool is_deleted = false;

            get_heap_tuple(&css->css, node->variable_name, &is_deleted);

            if (is_deleted || !entity_exists(estate, css->graph_oid, DATUM_GET_GRAPHID(id)))
                ereport(ERROR,
                    (errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
                     errmsg("vertex assigned to variable %s was deleted", node->variable_name)));
        }

        if (CYPHER_TARGET_NODE_IN_PATH(node->flags))
        {
            Datum vertex = scanTupleSlot->tts_values[node->tuple_position - 1];
            css->path_values = lappend(css->path_values, DatumGetPointer(vertex));
        }
    }

    // If the path continues, create the next edge, passing the vertex's id.
    if (next != NULL)
    {
        create_edge(css, lfirst(next), id, lnext(next));
    }

    return id;
}

/*
 * Find out if the entity still exists. This is for 'implicit' deletion
 * of an entity.
 */
static bool entity_exists(EState *estate, Oid graph_oid, graphid id)
{
    label_cache_data *label;
    ScanKeyData scan_keys[1];
    HeapScanDesc scan_desc;
    HeapTuple tuple;
    Relation rel;
    bool result = true;

    /*
     * Extract the label id from the graph id and get the table name
     * the entity is part of.
     */
    label = search_label_graph_id_cache(graph_oid, GET_LABEL_ID(id));

    // Setup the scan key to be the graphid
    ScanKeyInit(&scan_keys[0], 1, BTEqualStrategyNumber,
                F_GRAPHIDEQ, GRAPHID_GET_DATUM(id));

    rel = heap_open(label->relation, RowExclusiveLock);
    scan_desc = heap_beginscan(rel, estate->es_snapshot, 1, scan_keys);

    tuple = heap_getnext(scan_desc, ForwardScanDirection);

    /*
     * If a single tuple was returned, the tuple is still valid, otherwise'
     * set to false.
     */
    if (!HeapTupleIsValid(tuple))
        result = false;

    heap_endscan(scan_desc);
    heap_close(rel, RowExclusiveLock);

    return result;
}

/*
 * Insert the edge/vertex tuple into the table and indices. If the table's
 * constraints have not been violated.
 */
static HeapTuple insert_entity_tuple(ResultRelInfo *resultRelInfo,
                                      TupleTableSlot *elemTupleSlot, EState *estate)
{
    HeapTuple tuple;

    ExecStoreVirtualTuple(elemTupleSlot);
    tuple = ExecMaterializeSlot(elemTupleSlot);

    // Check the constraints of the tuple
    tuple->t_tableOid = RelationGetRelid(resultRelInfo->ri_RelationDesc);
    if (resultRelInfo->ri_RelationDesc->rd_att->constr != NULL)
        ExecConstraints(resultRelInfo, elemTupleSlot, estate);

    // Insert the tuple normally
    heap_insert(resultRelInfo->ri_RelationDesc, tuple, estate->es_output_cid,
                0, NULL);

    // Insert index entries for the tuple
    if (resultRelInfo->ri_NumIndices > 0)
        ExecInsertIndexTuples(elemTupleSlot, &(tuple->t_self), estate, false,
                              NULL, NIL);

    return tuple;
}
