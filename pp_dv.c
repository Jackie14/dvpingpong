#include <immintrin.h>
#include <pthread.h>
#include "mlx5_ifc.h"
#include "pp_dv.h"

#include "ccan/ilog.h"
#include "ccan/minmax.h"

#include "util/mmio.h"
#include "util/udma_barrier.h"
#include "util/util.h"
extern pthread_mutex_t lock;

#define DVDBG printf
#define DVDBG2(fmt, args...)

#define PP_ACCESS_FALGS (IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_WRITE | IBV_ACCESS_REMOTE_READ)

enum {
	MLX5_GEN_OBJ_TYPE_MKEY = 0xff01,
};
static struct mlx5dv_devx_obj *buf_mkey_obj_create(struct ibv_context *ibv_ctx,
						       uint32_t pd_id,
						       uint32_t umem_id,
						       uint8_t *buf,
						       size_t buf_size,
						       uint32_t *mkey)
{
	uint32_t out[DEVX_ST_SZ_DW(create_mkey_out)] = {0};
	uint32_t in[DEVX_ST_SZ_DW(create_mkey_in)] = {0};
	void *mkc = DEVX_ADDR_OF(create_mkey_in, in, memory_key_mkey_entry);
	size_t pgsize = sysconf(_SC_PAGESIZE);
	uint64_t aligned_mkey_obj = (buf_size + pgsize - 1) & ~(pgsize - 1);
	uint32_t translation_size = (aligned_mkey_obj * 8) / 16;
	uint32_t log_page_size = __builtin_ffs(pgsize) - 1;

	DEVX_SET(create_mkey_in, in, opcode, MLX5_CMD_OP_CREATE_MKEY);
	DEVX_SET(create_mkey_in, in, translations_octword_actual_size, translation_size);
	DEVX_SET(create_mkey_in, in, mkey_umem_id, umem_id);
	DEVX_SET(mkc, mkc, access_mode_1_0, 0x1);
	DEVX_SET(mkc, mkc, lw, 0x1);
	DEVX_SET(mkc, mkc, lr, 0x1);
	DEVX_SET(mkc, mkc, rw, 0x1);
	DEVX_SET(mkc, mkc, rr, 0x1);
	DEVX_SET(mkc, mkc, qpn, 0xffffff);
	DEVX_SET(mkc, mkc, pd, pd_id);
	DEVX_SET(mkc, mkc, mkey_7_0, umem_id & 0xFF);
	DEVX_SET(mkc, mkc, log_entity_size, log_page_size);
	DEVX_SET(mkc, mkc, translations_octword_size, translation_size);
	DEVX_SET64(mkc, mkc, start_addr, (uint64_t)buf);
	DEVX_SET64(mkc, mkc, len, buf_size);

	struct mlx5dv_devx_obj *obj = mlx5dv_devx_obj_create(ibv_ctx, in, sizeof(in), out, sizeof(out));
	if (!obj) {
		ERR("devx obj create failed %d", errno);
		return NULL;
	}

	*mkey = (DEVX_GET(create_mkey_out, out, mkey_index) << 8) | (umem_id & 0xFF);
	return obj;
}

bool allow_other_vhca_access(struct ibv_context *ibctx,
			     const uint8_t *access_key,
		      	     size_t access_key_size,
			     uint32_t lkey)
{
	uint32_t in[DEVX_ST_SZ_DW(allow_other_vhca_access_in)] = {};
	uint32_t out[DEVX_ST_SZ_DW(allow_other_vhca_access_out)] = {};

	/* Allow crossing mkey */
	//DEVX_SET(allow_other_vhca_access_in, in, uid, 0);
	DEVX_SET(allow_other_vhca_access_in, in, opcode, MLX5_CMD_OPCODE_ALLOW_OTHER_VHCA_ACCESS);
	DEVX_SET(allow_other_vhca_access_in, in, object_type_to_be_accessed, MLX5_GEN_OBJ_TYPE_MKEY);
	DEVX_SET(allow_other_vhca_access_in, in, object_id_to_be_accessed, lkey >> 8);
	void *access_key_dest = DEVX_ADDR_OF(allow_other_vhca_access_in, in, access_key);
	memcpy(access_key_dest, access_key, access_key_size);

	int err = mlx5dv_devx_general_cmd(ibctx, in, sizeof(in), out, sizeof(out));
	if (err) {
		int status, syndrome;
		status = DEVX_GET(allow_other_vhca_access_out, out, status);
		syndrome = DEVX_GET(allow_other_vhca_access_out, out, syndrome);
		ERR("allow_other_vhca_access failed, status:%x, syndrome:%x\n", status, syndrome);
		return false;
	}
	//struct mlx5dv_devx_obj *obj = mlx5dv_devx_obj_create(ibctx, in, sizeof(in), out, sizeof(out));
	//if (!obj) {
	//	int status, syndrome;
	//	status = DEVX_GET(allow_other_vhca_access_out, out, status);
	//	syndrome = DEVX_GET(allow_other_vhca_access_out, out, syndrome);
	//	ERR("allow_other_vhca_access failed, status:%x, syndrome:%x\n", status, syndrome);
	//	return false;
	//}
	return true;
}
struct mlx5dv_devx_obj *create_alias_mkey_obj(struct ibv_context *ibctx,
		      uint16_t vhca_id,
		      uint32_t lkey,
		      const uint8_t *access_key,
		      size_t access_key_size,
		      uint32_t pdn,
		      uint32_t mkey_modify_cqn,
		      uint32_t *alias_mkey)
{
	uint32_t in[DEVX_ST_SZ_DW(create_alias_obj_in)] = {0};
	uint32_t out[DEVX_ST_SZ_DW(create_alias_obj_out)] = {0};

	void *hdr = DEVX_ADDR_OF(create_alias_obj_in, in, hdr);
	void *alias_ctx = DEVX_ADDR_OF(create_alias_obj_in, in, alias_ctx);
	void *op_param = DEVX_ADDR_OF(general_obj_in_cmd_hdr, in, op_param);

	DEVX_SET(general_obj_in_cmd_hdr, hdr, opcode, MLX5_CMD_OP_CREATE_GENERAL_OBJECT);
	DEVX_SET(general_obj_in_cmd_hdr, hdr, obj_type, MLX5_GEN_OBJ_TYPE_MKEY);
	DEVX_SET(general_obj_create_param, op_param, alias_object, 1);

	DEVX_SET(alias_context, alias_ctx, vhca_id_to_be_accessed, vhca_id);
	DEVX_SET(alias_context, alias_ctx, object_id_to_be_accessed, lkey >> 8);


	// How this access key edited?
	void *access_key_addr = DEVX_ADDR_OF(alias_context, alias_ctx, access_key);
	memcpy(access_key_addr, access_key, access_key_size);

	// what is this pdn
	void *metadata = DEVX_ADDR_OF(alias_context, alias_ctx, alias_metadata);
	DEVX_SET(alias_ctx_metadata_mkey, metadata, pdn, pdn);
	DEVX_SET(alias_ctx_metadata_mkey, metadata, mkey_error_modify_en, 1);
	DEVX_SET(alias_ctx_metadata_mkey, metadata, mkey_error_modify_cqn, mkey_modify_cqn);

	struct mlx5dv_devx_obj *obj = mlx5dv_devx_obj_create(ibctx, in, sizeof(in), out, sizeof(out));
	if (!obj) {
		int status, syndrome;
		status = DEVX_GET(allow_other_vhca_access_out, out, status);
		syndrome = DEVX_GET(allow_other_vhca_access_out, out, syndrome);
		ERR("devx_obj_create(alias mkey) failed. status:%x, syndrome:%x\n", status, syndrome);
		return NULL;
	}

	*alias_mkey = (DEVX_GET(create_alias_obj_out, out, hdr.obj_id) << 8);
	return obj;
}

//int dr_devx_query_gvmi(struct ibv_context *ctx, bool other_vport,
//		       uint16_t vport_number, uint16_t *gvmi)
int dr_devx_query_gvmi(struct ibv_context *ctx, uint16_t *gvmi)
{
	uint32_t out[DEVX_ST_SZ_DW(query_hca_cap_out)] = {};
	uint32_t in[DEVX_ST_SZ_DW(query_hca_cap_in)] = {};
	int err;

	DEVX_SET(query_hca_cap_in, in, opcode, MLX5_CMD_OP_QUERY_HCA_CAP);
	//DEVX_SET(query_hca_cap_in, in, other_function, other_vport);
	//DEVX_SET(query_hca_cap_in, in, function_id, vport_number);
	DEVX_SET(query_hca_cap_in, in, op_mod,
		 MLX5_SET_HCA_CAP_OP_MOD_GENERAL_DEVICE |
		 HCA_CAP_OPMOD_GET_CUR);

	err = mlx5dv_devx_general_cmd(ctx, in, sizeof(in), out, sizeof(out));
	if (err) {
		//err = mlx5_get_cmd_status_err(err, out);
		//ERR("Query general failed %d\n", err);
		ERR("Query general failed\n");
		return err;
	}

	*gvmi = DEVX_GET(query_hca_cap_out, out, capability.cmd_hca_cap.vhca_id);

	return 0;
}

uint32_t get_pdn(struct ibv_pd *pd)
{
	struct mlx5dv_pd dv_pd = {};
	struct mlx5dv_obj obj = {};
	int ret;

	obj.pd.in = pd;
	obj.pd.out = &dv_pd;
	ret = mlx5dv_init_obj(&obj, MLX5DV_OBJ_PD);
	if (ret) {
		ERR("mlx5dv_init_obj(PD) failed\n");
		return 0xffffffff;
	}
	return dv_pd.pdn;
}


int pp_create_cq_dv(const struct pp_context *ppc, struct pp_dv_cq *dvcq)
{
	uint32_t in[DEVX_ST_SZ_DW(create_cq_in)] = {};
	uint32_t out[DEVX_ST_SZ_DW(create_cq_out)] = {};
	void *cqc = DEVX_ADDR_OF(create_cq_in, in, cq_context);
	struct mlx5_cqe64 *cqe;
	uint32_t eqn;
	int i, ret;

	dvcq->cqe_sz = 64;
	dvcq->ncqe = 1 << PP_MAX_LOG_CQ_SIZE_2;

	ret = mlx5dv_devx_query_eqn(ppc->ibctx, 0, &eqn);
	if (ret) {
		ERR("devx_query_eqn failed: %d, errno %d\n", ret, errno);
		return ret;
	}

	ret = posix_memalign((void **)&dvcq->db, 8, 8);
	if (ret) {
		ERR("cq.db posix_memalign(8) failed\n");
		return ret;
	}

	dvcq->db[0] = 0;
	dvcq->db[1] = 0;
	dvcq->db_umem = mlx5dv_devx_umem_reg(ppc->ibctx, dvcq->db, 8,
					     PP_ACCESS_FALGS);
	if (!dvcq->db_umem) {
		ERR("cq.db umem_reg() failed\n");
		goto fail;
	}

	//int size = 4096 * 8;	/* FIXME: How to calculate it? (cq_size * cqe_size)? */
	int size = roundup_pow_of_two(dvcq->cqe_sz * dvcq->ncqe);
	dvcq->buflen = align(size, sysconf(_SC_PAGESIZE));
	ret = posix_memalign(&dvcq->buf, sysconf(_SC_PAGESIZE), dvcq->buflen);
	if (ret) {
		ERR("cq.buf posix_memalign(0x%lx) failed\n", dvcq->buflen);
		goto fail_alloc_cqbuf;
	}

	memset(dvcq->buf, 0, dvcq->buflen);
	dvcq->buff_umem = mlx5dv_devx_umem_reg(ppc->ibctx, dvcq->buf,
					       dvcq->buflen, PP_ACCESS_FALGS);
	if (!dvcq->buff_umem) {
		ERR("cq.buf umem_reg(0x%lx) failed\n", dvcq->buflen);
		goto fail_umem_reg_cqbuf;
	}

	dvcq->uar = mlx5dv_devx_alloc_uar(ppc->ibctx, MLX5_IB_UAPI_UAR_ALLOC_TYPE_NC);
	if (!dvcq->uar) {
		ERR("cq alloc_uar() failed\n");
		goto fail_alloc_uar;
	}

	DEVX_SET(create_cq_in, in, opcode, MLX5_CMD_OP_CREATE_CQ);

	//DEVX_SET64(cqc, cqc, log_page_size, 0); /* 12 - MLX5_ADAPTER_PAGE_SHIFT */
	//DEVX_SET64(cqc, cqc, page_offset, 0);
	DEVX_SET(cqc, cqc, log_page_size, 0); /* 12 - MLX5_ADAPTER_PAGE_SHIFT */
	DEVX_SET(cqc, cqc, page_offset, 0);
	DEVX_SET(cqc, cqc, log_cq_size, PP_MAX_LOG_CQ_SIZE_2);
	DEVX_SET(cqc, cqc, cqe_sz, 0); /* BYTES_64 */
	DEVX_SET(cqc, cqc, oi, 1);

	/* FIXME: Check these args */
	DEVX_SET(cqc, cqc, uar_page, dvcq->uar->page_id);
	DEVX_SET(cqc, cqc, c_eqn, eqn);

	//DEVX_SET64(cqc, cqc, dbr_umem_id, dvcq->db_umem->umem_id);
	//DEVX_SET64(cqc, cqc, dbr_umem_valid, 1);
	//DEVX_SET64(cqc, cqc, dbr_addr, 0);
	DEVX_SET(cqc, cqc, dbr_umem_id, dvcq->db_umem->umem_id);
	DEVX_SET(cqc, cqc, dbr_umem_valid, 1);
	DEVX_SET(cqc, cqc, dbr_addr, 0);



	DEVX_SET(create_cq_in, in, cq_umem_id, dvcq->buff_umem->umem_id);
	DEVX_SET(create_cq_in, in, cq_umem_valid, 1);

	/*
	for (int i = 0; i < sizeof(in)/4; i++) {
		DVDBG("%03x: ", (i * 4));
		for (int j = 0; j < 4; j++) {
			DVDBG("%02x ", *(((uint8_t *)in) + (i * 4) + j));
		}
		DVDBG("\n");
	}
	*/

	dvcq->obj = mlx5dv_devx_obj_create(ppc->ibctx, in, sizeof(in), out, sizeof(out));
	if (!dvcq->obj) {
		int status, syndrome;
		status = DEVX_GET(create_cq_out, out, status);
		syndrome = DEVX_GET(create_cq_out, out, syndrome);
		ERR("devx_obj_create(cq) failed: eqn %#x, status %#x, syndrome %#x, dbr_umem_id %#x, buf_umem_id %#x\n", eqn, status, syndrome, dvcq->db_umem->umem_id, dvcq->buff_umem->umem_id);
		goto fail_obj_create;
	}
	dvcq->cqn = DEVX_GET(create_cq_out, out, cqn);
	INFO("dv: CQ %#x created, eqn %#x, db@%p %#x, buf@%p, %#x\n",
	     dvcq->cqn, eqn, dvcq->db, dvcq->db_umem->umem_id, dvcq->buf, dvcq->buff_umem->umem_id);

	dvcq->cons_index = 0;
	for (i = 0; i < dvcq->ncqe; i++) {
		cqe = pp_dv_get_cqe(dvcq, i);
		cqe->op_own = MLX5_CQE_INVALID << 4;
	}

	return 0;

fail_obj_create:
	mlx5dv_devx_free_uar(dvcq->uar);
fail_alloc_uar:
	mlx5dv_devx_umem_dereg(dvcq->buff_umem);
fail_umem_reg_cqbuf:
	free(dvcq->buf);
	dvcq->buf = NULL;
fail_alloc_cqbuf:
	mlx5dv_devx_umem_dereg(dvcq->db_umem);
fail:
	free(dvcq->db);
	dvcq->db = NULL;

	return -1;
}

void pp_destroy_cq_dv(struct pp_dv_cq *dvcq)
{
	mlx5dv_devx_obj_destroy(dvcq->obj);
	mlx5dv_devx_free_uar(dvcq->uar);
	mlx5dv_devx_umem_dereg(dvcq->buff_umem);
	free(dvcq->buf);
	dvcq->buf = NULL;
	mlx5dv_devx_umem_dereg(dvcq->db_umem);
	free(dvcq->db);
	dvcq->db = NULL;
}

static int calc_rc_send_wqe(const struct ibv_qp_cap *qp_cap)
{
	int size, inl_size, tot_size;

	//size = sizeof(struct mlx5_wqe_ctrl_seg) + sizeof(struct mlx5_wqe_raddr_seg);
	size = 192;		/* Ref. providers/mlx5/verb.c::sq_overhead() */
	if (qp_cap->max_inline_data)
		inl_size = size + align(sizeof(struct mlx5_wqe_inl_data_seg) + qp_cap->max_inline_data, 16);

	/* 8.9.4.1.14 Send Data Segments */
	size += qp_cap->max_send_sge * sizeof(struct mlx5_wqe_data_seg);
	tot_size = max(size, inl_size);
	return align(tot_size, MLX5_SEND_WQE_BB);
}

static int calc_sq_size(struct dv_wq *sq, const struct ibv_qp_cap *qp_cap)
{
	int wqe_size;
	int wq_size;

	wqe_size = calc_rc_send_wqe(qp_cap);
	wq_size = roundup_pow_of_two(qp_cap->max_send_wr * wqe_size);
	sq->wqe_cnt = wq_size / MLX5_SEND_WQE_BB;
	sq->wqe_shift = STATIC_ILOG_32(MLX5_SEND_WQE_BB) - 1;
	sq->max_gs = qp_cap->max_send_sge;
	sq->max_post = wq_size / wqe_size;

	DVDBG("    sq: wqe_size %d(0x%x), wq_size %d(0x%x), wqe_cnt %d, wqe_shift %d, max_gs %d, max_post %d\n",
	       wqe_size, wqe_size, wq_size, wq_size, sq->wqe_cnt, sq->wqe_shift, sq->max_gs, sq->max_post);
	return wq_size;
}

static int calc_recv_wqe(const struct ibv_qp_cap *qp_cap)
{
	int size, num_scatter;

	num_scatter = max_t(uint32_t, qp_cap->max_recv_sge, 1);
	size = sizeof(struct mlx5_wqe_data_seg) * num_scatter;
	size = roundup_pow_of_two(size);

	return size;
}

static int calc_rq_size(struct dv_wq *rq, const struct ibv_qp_cap *qp_cap)
{
	int wqe_size;
	int wq_size;

	wqe_size = calc_recv_wqe(qp_cap);

	wq_size = roundup_pow_of_two(qp_cap->max_recv_wr) * wqe_size;
	wq_size = max(wq_size, MLX5_SEND_WQE_BB);
	rq->wqe_cnt = wq_size / wqe_size;
	rq->wqe_shift = ilog32(wqe_size - 1);
	rq->max_post = 1 << ilog32(wq_size / wqe_size - 1);
	rq->max_gs = wqe_size / sizeof(struct mlx5_wqe_data_seg);

	DVDBG("    rq: wqe_size %d(0x%x), wq_size %d(0x%x), wqe_cnt %d, wqe_shift %d, max_gs %d, max_post %d\n",
	      wqe_size, wqe_size, wq_size, wq_size, rq->wqe_cnt, rq->wqe_shift, rq->max_gs, rq->max_post);
	return wq_size;
}

static int calc_wq_size(struct pp_dv_qp *dvqp, const struct ibv_qp_cap *cap)
{
	int result;
	int ret;

	DVDBG("    qp_cap: .max_send_wr %d, max_recv_wr %d, max_send_sge %d, max_recv_sge %d, max_inline_data %d\n",
	      cap->max_send_wr, cap->max_recv_wr, cap->max_send_sge, cap->max_recv_sge, cap->max_inline_data);

	result = calc_sq_size(&dvqp->sq, cap);
	ret = calc_rq_size(&dvqp->rq, cap);

	result += ret;
	dvqp->rq.offset = 0;
	dvqp->sq.offset = ret;
/*
	dvqp->max_inline_data =
		calc_rc_send_wqe(&dr_qp->cap) -
		(sizeof(struct  mlx5_wqe_ctrl_seg) + sizeof(struct mlx5_wqe_raddr_seg)) -
		sizeof(struct mlx5_wqe_inl_data_seg);
*/
	DVDBG("   wq total %d(0x%x) sq.offset %d(0x%x)\n\n",
	      result, result, dvqp->sq.offset, dvqp->sq.offset);
	return result;
}

int pp_create_qp_dv(const struct pp_context *ppc,
		    struct pp_dv_cq *dvcq, struct pp_dv_qp *dvqp)
{
	uint32_t in[DEVX_ST_SZ_DW(create_qp_in)] = {};
	uint32_t out[DEVX_ST_SZ_DW(create_qp_out)] = {};
	void *qpc = DEVX_ADDR_OF(create_qp_in, in, qpc);
	struct mlx5dv_pd dv_pd = {};
	struct mlx5dv_obj obj = {};
	int size, ret;

	obj.pd.in = ppc->pd;
	obj.pd.out = &dv_pd;
	ret = mlx5dv_init_obj(&obj, MLX5DV_OBJ_PD);
	if (ret) {
		ERR("mlx5dv_init_obj(PD) failed\n");
		return ret;
	}

	ret = posix_memalign((void **)&dvqp->db, 8, 8);
	if (ret) {
		ERR("qp.db posix_memalign(8) failed\n");
		return ret;
	}

	dvqp->db[0] = 0;
	dvqp->db[1] = 0;
	dvqp->db_umem = mlx5dv_devx_umem_reg(ppc->ibctx, dvqp->db, 8,
					     PP_ACCESS_FALGS);
	if (!dvqp->db_umem) {
		ERR("qp.db umem_reg() failed\n");
		goto fail;
	}

	size = calc_wq_size(dvqp, &ppc->cap);
	dvqp->buflen = align(size, sysconf(_SC_PAGESIZE));
	ret = posix_memalign(&dvqp->buf, sysconf(_SC_PAGESIZE), dvqp->buflen);
	if (ret) {
		ERR("qp.buf posix_memalign(0x%lx) failed\n", dvqp->buflen);
		goto fail_alloc_qpbuf;
	}

	memset(dvqp->buf, 0, dvqp->buflen);
	dvqp->buff_umem = mlx5dv_devx_umem_reg(ppc->ibctx, dvqp->buf,
					       dvqp->buflen, PP_ACCESS_FALGS);
	if (!dvqp->buff_umem) {
		ERR("qp.buf umem_reg(0x%lx) failed\n", dvqp->buflen);
		goto fail_umem_reg_qpbuf;
	}

	dvqp->uar = mlx5dv_devx_alloc_uar(ppc->ibctx, MLX5_IB_UAPI_UAR_ALLOC_TYPE_NC);
	if (!dvqp->uar) {
		ERR("qp alloc_uar() failed\n");
		goto fail_alloc_uar;
	}

	DEVX_SET(create_qp_in, in, opcode, MLX5_CMD_OP_CREATE_QP);
        DEVX_SET(qpc, qpc, st, MLX5_QPC_ST_RC);
	DEVX_SET(qpc, qpc, pd, dv_pd.pdn);
	DEVX_SET(qpc, qpc, pm_state, MLX5_QPC_PM_STATE_MIGRATED);
	DEVX_SET(qpc, qpc, log_rq_size, ilog32(dvqp->rq.wqe_cnt - 1));
	DEVX_SET(qpc, qpc, log_rq_stride, dvqp->rq.wqe_shift - 4);
	DEVX_SET(qpc, qpc, log_sq_size, ilog32(dvqp->sq.wqe_cnt - 1));
	DEVX_SET(qpc, qpc, no_sq, 0);
	DEVX_SET(qpc, qpc, rlky, 0);
	DEVX_SET(qpc, qpc, uar_page, dvqp->uar->page_id);
	DEVX_SET(qpc, qpc, log_page_size, 0); /* 12 - MLX5_ADAPTER_PAGE_SHIFT */
	DEVX_SET(qpc, qpc, fre, 0);

	DEVX_SET(qpc, qpc, cqn_snd, dvcq->cqn);
	DEVX_SET(qpc, qpc, cqn_rcv, dvcq->cqn);

	DEVX_SET(qpc, qpc, page_offset, 0);

	DEVX_SET(qpc, qpc, dbr_addr, 0);
	DEVX_SET(qpc, qpc, rq_type, 0); /* NON_ZERO_RQ */

	DEVX_SET(qpc, qpc, dbr_umem_id, dvqp->db_umem->umem_id);
	DEVX_SET(qpc, qpc, dbr_umem_valid, 1);
	DEVX_SET(qpc, qpc, isolate_vl_tc, 0);

	DEVX_SET(create_qp_in, in, wq_umem_id, dvqp->buff_umem->umem_id);
	DEVX_SET(create_qp_in, in, wq_umem_valid, 1);

	dvqp->obj = mlx5dv_devx_obj_create(ppc->ibctx, in, sizeof(in), out, sizeof(out));
	if (!dvqp->obj) {
		ERR("devx_obj_create(qp) failed\n");
		goto fail_obj_create;
	}

	dvqp->qpn = DEVX_GET(create_qp_out, out, qpn);

	dvqp->sq_start = dvqp->buf + dvqp->sq.offset;
	dvqp->sq.qend = dvqp->buf + dvqp->sq.offset + (dvqp->sq.wqe_cnt << dvqp->sq.wqe_shift);
	dvqp->sq.cur_post = 0;
	dvqp->rq.head = 0;
	dvqp->rq.tail = 0;

	dvcq->dvqp = dvqp;

	INFO("dv: QP %#x created; sq.wqe_cnt %d(log_sq_size %d), rq.wqe_cnt %d(log_rq_size %d), rq_wqe_shift %d\n",
	     dvqp->qpn, dvqp->sq.wqe_cnt, ilog32(dvqp->sq.wqe_cnt - 1),
	     dvqp->rq.wqe_cnt, ilog32(dvqp->rq.wqe_cnt - 1), dvqp->rq.wqe_shift);

	return 0;

fail_obj_create:
	mlx5dv_devx_free_uar(dvqp->uar);
fail_alloc_uar:
	mlx5dv_devx_umem_dereg(dvqp->buff_umem);
fail_umem_reg_qpbuf:
	free(dvqp->buf);
	dvqp->buf = NULL;
fail_alloc_qpbuf:
	mlx5dv_devx_umem_dereg(dvqp->db_umem);
fail:
	free(dvqp->db);
	dvqp->db = NULL;

	return -1;
}

void pp_destroy_qp_dv(struct pp_dv_qp *dvqp)
{
	mlx5dv_devx_obj_destroy(dvqp->obj);
	mlx5dv_devx_free_uar(dvqp->uar);
	mlx5dv_devx_umem_dereg(dvqp->buff_umem);
	free(dvqp->buf);
	dvqp->buf = NULL;
	mlx5dv_devx_umem_dereg(dvqp->db_umem);
	free(dvqp->db);
	dvqp->db = NULL;
}

static int dvqp_rst2init(const struct pp_context *ppc, struct pp_dv_qp *dvqp)
{
	uint32_t out[DEVX_ST_SZ_DW(rst2init_qp_out)] = {};
	uint32_t in[DEVX_ST_SZ_DW(rst2init_qp_in)] = {};
	void *qpc = DEVX_ADDR_OF(rst2init_qp_in, in, qpc);
	int ret;

	DEVX_SET(rst2init_qp_in, in, opcode, MLX5_CMD_OP_RST2INIT_QP);
	DEVX_SET(rst2init_qp_in, in, qpn, dvqp->qpn);

	DEVX_SET(qpc, qpc, pm_state, MLX5_QPC_PM_STATE_MIGRATED);
	DEVX_SET(qpc, qpc, primary_address_path.vhca_port_num, ppc->port_num);
	DEVX_SET(qpc, qpc, rre, 1);
	DEVX_SET(qpc, qpc, rwe, 1);
	//DEVX_SET(qpc, qpc, lag_tx_port_affinity, 1);
	//DEVX_SET(qpc, qpc, primary_address_path.pkey_index, 0);
	//DEVX_SET(qpc, qpc, counter_set_id, 0);

	ret = mlx5dv_devx_obj_modify(dvqp->obj, in,
				     sizeof(in), out, sizeof(out));
	if (ret) {
		ERR("Failed to move qp %d to INIT state, port_num %d\n",
		    dvqp->qpn, ppc->port_num);
		return ret;
	}

	INFO("qp %#x moved to INIT state, port_num %d\n", dvqp->qpn, ppc->port_num);
	return 0;
}

/* FIXME: For RoCE currently dmac is hard-coded */
//static uint8_t dmac[6] = {0x02, 0x72, 0x18, 0xd5, 0x8d, 0x9b};
static uint8_t dmac[6] = {0xa0, 0x88, 0xc2, 0x53, 0x01, 0x80};
//static uint8_t dmac[6] = {0xec, 0x0d, 0x9a, 0x8a, 0x28, 0x2a};
static int dvqp_init2rtr(const struct pp_context *ppc,
			 const struct pp_exchange_info *peer,
			 int my_sgid_idx, struct pp_dv_qp *dvqp)
{
	uint32_t out[DEVX_ST_SZ_DW(init2rtr_qp_out)] = {};
	uint32_t in[DEVX_ST_SZ_DW(init2rtr_qp_in)] = {};
	void *qpc = DEVX_ADDR_OF(init2rtr_qp_in, in, qpc);
	void *pri_path =  DEVX_ADDR_OF(qpc, qpc, primary_address_path);
	int ret;

	DEVX_SET(init2rtr_qp_in, in, opcode, MLX5_CMD_OP_INIT2RTR_QP);
	DEVX_SET(init2rtr_qp_in, in, qpn, dvqp->qpn);

	DEVX_SET(qpc, qpc, mtu, IBV_MTU_1024);
	DEVX_SET(qpc, qpc, log_msg_max, 30);
	DEVX_SET(qpc, qpc, remote_qpn, peer->qpn);
	DEVX_SET(qpc, qpc, next_rcv_psn, peer->psn);

	if (ppc->port_attr.link_layer == IBV_LINK_LAYER_ETHERNET) {
		memcpy(DEVX_ADDR_OF(qpc, qpc, primary_address_path.rmac_47_32),
		       dmac, sizeof(dmac));
		memcpy(DEVX_ADDR_OF(qpc, qpc, primary_address_path.rgid_rip),
		       peer->gid.raw, sizeof(peer->gid.raw));
		//DEVX_SET(qpc, qpc, primary_address_path.src_addr_index, peer->rc.sgid_idx);
		DEVX_SET(qpc, qpc, primary_address_path.src_addr_index, my_sgid_idx);
		DEVX_SET(qpc, qpc, primary_address_path.udp_sport, 0xccdd);
		DEVX_SET(ads, pri_path, hop_limit, 64);
	} else {		/* IB */
		DEVX_SET(ads, pri_path, rlid, peer->lid);
	}
	//DEVX_SET(ads, pri_path, stat_rate, 0);

	DEVX_SET(qpc, qpc, primary_address_path.vhca_port_num, ppc->port_num);
	DEVX_SET(qpc, qpc, min_rnr_nak, 12);

	ret = mlx5dv_devx_obj_modify(dvqp->obj, in,
				     sizeof(in), out, sizeof(out));
	if (ret) {
		ERR("Failed to move qp %d to RTR state, dmac %02x:%02x:%02x:%02x:%02x:%02x\n",
		    dvqp->qpn, dmac[0], dmac[1], dmac[2], dmac[3], dmac[4], dmac[5]);
		return ret;
	}

	INFO("qp %#x moved to RTR state, dmac(hard-coded) %02x:%02x:%02x:%02x:%02x:%02x, peer.qpn %d\n",
	     dvqp->qpn, dmac[0], dmac[1], dmac[2], dmac[3], dmac[4], dmac[5], peer->qpn);
	return 0;
}

static int dvqp_rtr2rts(const struct pp_context *ppc, uint32_t my_sq_psn,
			struct pp_dv_qp *dvqp)
{
	uint32_t out[DEVX_ST_SZ_DW(rtr2rts_qp_out)] = {};
	uint32_t in[DEVX_ST_SZ_DW(rtr2rts_qp_in)] = {};
	void *qpc = DEVX_ADDR_OF(rtr2rts_qp_in, in, qpc);
	void *pri_path =  DEVX_ADDR_OF(qpc, qpc, primary_address_path);
	int ret;

	DEVX_SET(rtr2rts_qp_in, in, opcode, MLX5_CMD_OP_RTR2RTS_QP);
	DEVX_SET(rtr2rts_qp_in, in, qpn, dvqp->qpn);

	DEVX_SET(qpc, qpc, log_ack_req_freq, 0);
	DEVX_SET(qpc, qpc, retry_count, 7);
	DEVX_SET(qpc, qpc, rnr_retry, 7);
	DEVX_SET(qpc, qpc, next_send_psn, my_sq_psn);
	DEVX_SET(qpc, qpc, primary_address_path.vhca_port_num, ppc->port_num);

	DEVX_SET(qpc, qpc, log_sra_max, 20);
	DEVX_SET(ads, pri_path, ack_timeout, 14);

	ret = mlx5dv_devx_obj_modify(dvqp->obj, in,
				     sizeof(in), out, sizeof(out));
	if (ret) {
		ERR("dv: Failed to move qp %d to RTS state\n", dvqp->qpn);
		return ret;
	}

	INFO("qp %#x moved to RTS state\n", dvqp->qpn);
	return 0;
}

int pp_move2rts_dv(struct pp_context *ppc, struct pp_dv_qp *dvqp,
		     int my_sgid_idx, uint32_t my_sq_psn,
		     struct pp_exchange_info *peer)
{
	int ret;

	ret = dvqp_rst2init(ppc, dvqp);
	if (ret)
		return ret;

	ret = dvqp_init2rtr(ppc, peer, my_sgid_idx, dvqp);
	if (ret)
		return ret;

	return dvqp_rtr2rts(ppc, my_sq_psn, dvqp);
}

static void set_raddr_seg(struct mlx5_wqe_raddr_seg *rseg,
			  void *remote_addr, uint32_t rkey)
{
	rseg->raddr = htobe64((uint64_t)remote_addr);
	rseg->rkey = htobe32(rkey);
	rseg->reserved = 0;
}

static void post_send_db(struct pp_dv_qp *dvqp, int size, void *ctrl)
{
	/*
	 * Make sure that descriptors are written before
	 * updating doorbell record and ringing the doorbell
	 */
	udma_to_device_barrier();
	dvqp->db[MLX5_SND_DBR] = htobe32(dvqp->sq.cur_post & 0xffff);

	udma_to_device_barrier();
	mmio_write64_be((uint8_t *)dvqp->uar->reg_addr, *(__be64 *)ctrl);
}

static void set_src_data(void *seg, int id, const struct pp_context *ppc)
{
	pthread_mutex_lock(&lock);
	if (ppc->mem_region_type == MEM_REGION_TYPE_MR)
		mlx5dv_set_data_seg(seg, ppc->mrbuflen, ppc->mr[id]->lkey, (uint64_t)ppc->mrbuf[id]);
	else if (ppc->mem_region_type == MEM_REGION_TYPE_DEVX)
		mlx5dv_set_data_seg(seg, ppc->mrbuflen, ppc->mkey[id], (uint64_t)ppc->mkey_mrbuf[id]);
	else if (ppc->mem_region_type == MEM_REGION_TYPE_ALIAS_VF0) {
		mlx5dv_set_data_seg(seg, ppc->mrbuflen, ppc->alias_mkey[0][id], (uint64_t)ppc->alias_mkey_mrbuf[0][id]);
		//INFO("Use alias mkey: %#x\n", ppc->alias_mkey[0][id]);
	}
	else if (ppc->mem_region_type == MEM_REGION_TYPE_ALIAS_VF1) {
		mlx5dv_set_data_seg(seg, ppc->mrbuflen, ppc->alias_mkey[1][id], (uint64_t)ppc->alias_mkey_mrbuf[1][id]);
		//INFO("Use alias mkey: %#x\n", ppc->alias_mkey[1][id]);
	}
	pthread_mutex_unlock(&lock);
}

static void post_send_one(const struct pp_context *ppc, struct pp_dv_qp *dvqp,
			  struct pp_exchange_info *peer, int id,
			  int opcode, uint32_t send_flags)

{
	void *qend = dvqp->sq.qend, *seg;
	struct mlx5_wqe_ctrl_seg *ctrl;
	uint32_t imm = 0x50607080 + id;
	int idx, size = 0;

	idx = dvqp->sq.cur_post & (dvqp->sq.wqe_cnt - 1);
	//INFO("sq idx: %d, cur: %d, wqe_count: %d\n", idx, dvqp->sq.cur_post, dvqp->sq.wqe_cnt);

	ctrl = dvqp->sq_start + (idx << MLX5_SEND_WQE_SHIFT);
	*(uint32_t *)((void *)ctrl + 8) = 0;
	ctrl->imm = htobe32(imm);
	ctrl->fm_ce_se = send_flags & IBV_SEND_SIGNALED ? MLX5_WQE_CTRL_CQ_UPDATE : 0;

	seg = ctrl;
	seg += sizeof(*ctrl);
	size = sizeof(*ctrl) / 16;

	if ((opcode == MLX5_OPCODE_RDMA_WRITE_IMM) ||
	    (opcode == MLX5_OPCODE_RDMA_WRITE) ||
	    ((opcode == MLX5_OPCODE_RDMA_READ))) {
		set_raddr_seg(seg, peer->addr[id], peer->mrkey[id]);
		seg += sizeof(struct mlx5_wqe_raddr_seg);
		size += sizeof(struct mlx5_wqe_raddr_seg) / 16;
	}

	if (unlikely(seg == qend))
		seg = dvqp->sq_start;
	set_src_data(seg, id, ppc);
	size += sizeof(struct mlx5_wqe_data_seg) / 16;

	ctrl->opmod_idx_opcode =
		htobe32(((dvqp->sq.cur_post & 0xffff) << 8) | opcode);
	ctrl->qpn_ds = htobe32(size | (dvqp->qpn << 8));

	dvqp->sq.cur_post += DIV_ROUND_UP(size * 16, MLX5_SEND_WQE_BB);

	post_send_db(dvqp, size, ctrl);
}

int pp_dv_post_send(const struct pp_context *ppc, struct pp_dv_qp *dvqp,
		    struct pp_exchange_info *peer, unsigned int num_post,
		    int opcode, uint32_t send_flags)
{
	int i;

	if (!num_post ||
	    (num_post > dvqp->sq.max_post) || (num_post > PP_MAX_WR)) {
		ERR("Invalid num_post %d (max %d)\n", num_post, dvqp->sq.max_post);
		return EINVAL;
	}

	if (send_flags & IBV_SEND_INLINE) {
		ERR("send flag 0x%x: IBV_SEND_INLINE is not supported\n", send_flags);
		return EINVAL;
	}

	for (i = 0; i < num_post; i++)
		post_send_one(ppc, dvqp, peer, i, opcode, send_flags);

	return 0;
}

void *pp_dv_get_cqe(struct pp_dv_cq *dvcq, int n)
{
	return dvcq->buf + n * dvcq->cqe_sz;
}

static void *get_sw_cqe(struct pp_dv_cq *dvcq, int n)
{
	void *cqe = pp_dv_get_cqe(dvcq, n & (dvcq->ncqe - 1));
	struct mlx5_cqe64 *cqe64;

	cqe64 = (dvcq->cqe_sz == 64) ? cqe : cqe + 64;

	DVDBG2("cqbuf %p n %d cqe_sz %d ncqe %d cqe64 %p owner 0x%x, opcde 0x%x final %d\n",
	       dvcq->buf, n, dvcq->cqe_sz, dvcq->ncqe, cqe64,
	       mlx5dv_get_cqe_owner(cqe64), mlx5dv_get_cqe_opcode(cqe64),
	       !((cqe64->op_own & MLX5_CQE_OWNER_MASK) ^ !!(n & dvcq->ncqe)));

	if (likely(mlx5dv_get_cqe_opcode(cqe64) != MLX5_CQE_INVALID) &&
	    !((cqe64->op_own & MLX5_CQE_OWNER_MASK) ^ !!(n & dvcq->ncqe))) {
	DVDBG2("cqbuf %p n %d cqe_sz %d ncqe %d cqe64 %p owner 0x%x, opcde 0x%x final %d\n",
	       dvcq->buf, n, dvcq->cqe_sz, dvcq->ncqe, cqe64,
	       mlx5dv_get_cqe_owner(cqe64), mlx5dv_get_cqe_opcode(cqe64),
	       !((cqe64->op_own & MLX5_CQE_OWNER_MASK) ^ !!(n & dvcq->ncqe)));
		return cqe64;
	    }
	else
		return NULL;
}

typedef uint32_t uint32 ;
struct cqe_error_syndrome_t {   /* Big Endian */
        uint32                  hw_error_syndrome:8;                    /* Mellanox specific error HW syndrome. See separate documentation. */
        uint32                  hw_syndrome_type:4;
        uint32                  reserved_at_c:4;
        uint32                  vendor_error_syndrome:8;                        /* Mellanox specific error syndrome. Undocumented.;For further information contact Mellanox technical support. */
        uint32                  syndrome:8;                     /* IB compliant completion with error syndrome;0x1: Local_Length_Error;0x2: Local_QP_Operation_Error;0x4: Local_Protection_Error;0x5: Work_Request_Flushed_Error ;0x6: Memory_Window_Bind_Error;0x10: Bad_Response_Error;0x11: Local_Access_Error;0x12: Remote_Invalid_Request_Error;0x13: Remote_Access_Error;0x14: Remote_Operation_Error;0x15: Transport_Retry_Counter_Exceeded;0x16: RNR_Retry_Counter_Exceeded;0x22: Aborted_Error;other is Reserved;Syndrome is defined according to the InfiniBand Architecture ;Specification, Volume 1. For a detailed explanation of the syn;dromes, refer to the Software Transport Interface and Soft;ware Transport Verbs chapters of the IB specification. */
/* --------------------------------------------------------- */
};
struct sw_cqe_mkey_err_t {      /* Big Endian */
        uint32                  cqe_subtype:8;                  /* CQE notification type. */
        uint32                  reserved_at_8:24;
        /*----------------------------------------------------------*/
        unsigned char                   reserved_at_20[12];
        /*----------------------------------------------------------*/
        uint32                  reserved_at_80:16;
        uint32                  mkey_modification_type:8;                       /* Defines the modification performed to the MKey. */
        uint32                  access_fail_type:8;                     /* Defines the operation that failed the memory access. */
        /*----------------------------------------------------------*/
        unsigned char                   reserved_at_a0[12];
        /*----------------------------------------------------------*/


	// ----- 32
        uint32                  reserved_at_100:8;
        uint32                  access_fail_srqn_rmpn_xrqn:24;                  /* SRQ/RMP/XRC_SRQ/XRQ number that failed the memory access. */
        /*----------------------------------------------------------*/
        uint32                  access_fail_mkey;                       /* MKey modified due to access error. */
        /*----------------------------------------------------------*/
        unsigned char                   reserved_at_140[12];
        /*----------------------------------------------------------*/
        struct cqe_error_syndrome_t                     cqe_error_syndrome;
        /*----------------------------------------------------------*/
        uint32                  send_wqe_opcode:8;                      /* On requester - the send WQE opcode. */
        uint32                  access_fail_qpn:24;                     /* QP that failed the memory access. */
        /*----------------------------------------------------------*/
        //uint32                  wqe_counter:16;                 /* WQE index that failed memory access. */
        //uint32                  signature:8;
        //uint32                  opcode:4;
        //uint32                  reserved_at_1fc:3;
        //uint32                  owner:1;

	uint16_t	wqe_counter;
	uint8_t		signature;
	uint8_t		op_own;
/* --------------------------------------------------------- */
};

static int parse_cqe(struct pp_dv_cq *dvcq, struct mlx5_cqe64 *cqe64)
{
//#if 0
	uint16_t wqe_ctr;
	uint8_t opcode;
	int idx;

	wqe_ctr = be16toh(cqe64->wqe_counter);
	opcode = mlx5dv_get_cqe_opcode(cqe64);

	if (opcode == MLX5_CQE_REQ_ERR || opcode == MLX5_CQE_RESP_ERR) {
		idx = wqe_ctr & ((opcode == MLX5_CQE_REQ_ERR ? dvcq->dvqp->sq.wqe_cnt : dvcq->dvqp->rq.wqe_cnt) - 1);

		struct mlx5_err_cqe *cqe_err = (struct mlx5_err_cqe *)cqe64;
		uint32_t wqe_opcode_qpn = be32toh(cqe_err->s_wqe_opcode_qpn);
		uint8_t vendor_err_synd = cqe_err->vendor_err_synd;
		uint8_t syndrome  = cqe_err->syndrome;
		DVDBG("Error %s, wqe_idx: %d, wqe_opcode_qpn: %x, syndrome: %x, vendor_err_synd: %x\n",
			(opcode == MLX5_CQE_REQ_ERR ? "MLX5_CQE_REQ_ERR" : "MLX5_CQE_RESP_ERR"),
			idx, wqe_opcode_qpn, syndrome, vendor_err_synd);
		// Error MLX5_CQE_REQ_ERR, wqe_idx: 0, wqe_opcode_qpn: b0002ab, syndrome: 15, vendor_err_synd: 81
		// b = 0xB: Send_with_Immediate
		// 2a8 = qpn
		/* 15 = syndrome:
		   IB compliant completion with error syndrome
		   0x1: Local_Length_Error
		   0x2: Local_QP_Operation_Error
		   0x4: Local_Protection_Error
		   0x5: Work_Request_Flushed_Error
		   0x6: Memory_Window_Bind_Error
		   0x10: Bad_Response_Error
		   0x11: Local_Access_Error
		   0x12: Remote_Invalid_Request_Error
		   0x13: Remote_Access_Error
		   0x14: Remote_Operation_Error
		   0x15: Transport_Retry_Counter_Exceeded.
		   0x16: RNR_Retry_Counter_Exceeded
		   0x22: Aborted_Error
		   other is Reserved
		   Syndrome is defined according to the InfiniBand Architecture
		   Specification, Volume 1. For a detailed explanation of the syndromes,
		   refer to the Software Transport Interface and Software
		   Transport Verbs chapters of the IB specification.
		*/
	} else if (opcode == 0xb) {
		struct sw_cqe_mkey_err_t *mkey_err = (struct sw_cqe_mkey_err_t *)cqe64;
		DVDBG("MLX5_CQE_MODIFY_MKEY\n");
		for (int i = 0; i < 16; i++) {
			for (int j = 0; j < 4; j++) {
				DVDBG("%02x ", *(((uint8_t *)cqe64) + (i * 4) + j));
			}
			DVDBG("\n");
		}
		DVDBG("\n");
		DVDBG("%-30s: %#x\n", "cqe_subtype", mkey_err->cqe_subtype);
		DVDBG("%-30s: %#x\n", "reserved_at_8", be32toh(mkey_err->reserved_at_8));

		DVDBG("%-30s: %#x\n", "mkey_modification_type", mkey_err->mkey_modification_type);
		DVDBG("%-30s: %#x\n","access_fail_type", mkey_err->access_fail_type);
		DVDBG("%-30s: %#x\n", "access_fail_srqn_rmpn_xrqn", be32toh(mkey_err->access_fail_srqn_rmpn_xrqn));
		DVDBG("%-30s: %#x\n", "access_fail_mkey", be32toh(mkey_err->access_fail_mkey));
		DVDBG("%-30s: %#x\n", "send_wqe_opcode", mkey_err->send_wqe_opcode);
		DVDBG("%-30s: %#x\n", "access_fail_qpn", be32toh(mkey_err->access_fail_qpn));

		DVDBG("%-30s: %#x\n", "hw_error_syndrome", mkey_err->cqe_error_syndrome.hw_error_syndrome);
		DVDBG("%-30s: %#x\n", "hw_syndrome_type", mkey_err->cqe_error_syndrome.hw_syndrome_type);
		DVDBG("%-30s: %#x\n", "reserved_at_c", mkey_err->cqe_error_syndrome.reserved_at_c);
		DVDBG("%-30s: %#x\n", "vendor_error_syndrome", mkey_err->cqe_error_syndrome.vendor_error_syndrome);
		DVDBG("%-30s: %#x\n", "syndrome", mkey_err->cqe_error_syndrome.syndrome);

		DVDBG("%-30s: %#x\n", "wqe_counter", be16toh(mkey_err->wqe_counter));
		//DVDBG("%-30s: %#x\n", "opcode", mkey_err->opcode);
		DVDBG("%-30s: %#x\n", "signature", mkey_err->signature);
		DVDBG("%-30s: %#x\n", "opcode", mkey_err->op_own  >> 4);
		//DVDBG("%-30s: %#x\n", "reserved_at_1fc", mkey_err->reserved_at_1fc);
		//DVDBG("%-30s: %#x\n", "owner", mkey_err->owner);

		dvcq->last_access_fail_mkey = be32toh(mkey_err->access_fail_mkey);
		return CQ_OK;
	} else {
		// TODO distinguish between send and recv
		//INFO("SET wqe_ci: %#x\n", wqe_ctr);
		dvcq->dvqp->sq.wqe_ci = wqe_ctr;
		return CQ_OK;
	}

	DVDBG("Error %u\n", opcode);
	return CQ_POLL_ERR;
//#endif
	return CQ_OK;
}

int pp_init_mkey(struct pp_context *pp)
{
	int i;
	pp->mrbuflen = PP_DATA_BUF_LEN;
	for (i = 0; i < PP_MAX_WR; i++) {
		uint32_t mkey = 0;
		struct mlx5dv_devx_obj *obj;
		obj = buf_mkey_obj_create(pp->ibctx, pp->pdn, pp->umem_obj[i]->umem_id,
					 pp->mkey_mrbuf[i], pp->mrbuflen, &mkey);
		if (!obj) {
			ERR("mkey create failed");
			goto fail_mkey_create;
		}

		uint8_t access_key[32];
		for (int i = 0; i < 32; i++) {
			access_key[i] = 'a' + i;
		}
		int ret = allow_other_vhca_access(pp->ibctx,
						access_key,
						32,
						mkey);
		if (ret != true) {
			ERR("failed to set other vhca access %d\n", i);
			goto fail_mkey_create;
		}

		pp->mkey_obj[i] = obj;
		pp->mkey[i] = mkey;
	
		INFO("%d devx mkey %#x\n", i, pp->mkey[i]);
	}

	return 0;

fail_mkey_create:
	for (i = 0; i < PP_MAX_WR; i++)
		if (pp->mkey_obj[i])
			mlx5dv_devx_obj_destroy(pp->mkey_obj[i]);
	return 0;

}

void pp_destroy_mkey(struct pp_context *pp)
{
	for (int i = 0; i < PP_MAX_WR; i++) {
		INFO("%d destroy devx mkey %#x\n", i, pp->mkey[i]);
		if (pp->mkey_obj[i]) {
			mlx5dv_devx_obj_destroy(pp->mkey_obj[i]);
			pp->mkey_obj[i] = NULL;
			pp->mkey[i] = 0;
		}
	}
}

void pp_destroy_obj(struct mlx5dv_devx_obj **obj)
{
	for (int i = 0; i < PP_MAX_WR; i++) {
		if (obj[i]) {
			mlx5dv_devx_obj_destroy(obj[i]);
			obj[i] = NULL;
		}
	}
}

int pp_allow_other_vhca_access(struct pp_context *pp)
{
	for (int i = 0; i < PP_MAX_WR; i++) {
		uint8_t access_key[32];
		for (int i = 0; i < 32; i++) {
			access_key[i] = 'a' + i;
		}
		int ret = allow_other_vhca_access(pp->ibctx, access_key, 32, pp->mkey[i]);
		if (ret != true) {
			ERR("failed to set other vhca access %d\n", i);
			return 1;
		}
	}

	return 0;
}

extern struct pp_dv_ctx ppdv;
int pp_init_alias_mkey(struct pp_context *pp, struct pp_context *target_pp, uint32_t modify_mkey_cqn, int vf_idx) {
	// Set the mkey to be accesseable by alias mkey
	uint8_t access_key[32];
	for (int i = 0; i < 32; i++) {
		access_key[i] = 'a' + i;
	}

	for (int i = 0; i < PP_MAX_WR; i++) {
		// We should create new alias mkey firstly, then destroy the old one.
		// to avoid QP using this old alias mkey crash.
		struct mlx5dv_devx_obj *old_alias_mkey_obj = pp->alias_mkey_obj[vf_idx][i];
		uint32 old_alias_mkey = pp->alias_mkey[vf_idx][i];

		uint32_t alias_mkey;
		struct mlx5dv_devx_obj *obj = create_alias_mkey_obj(pp->ibctx,
					target_pp->vhca_id,
					target_pp->mkey[i],
					access_key,
					32,
					pp->pdn,	
					modify_mkey_cqn,
					&alias_mkey);
		if (obj == NULL) {
			return 3;
		}
		INFO("Create alias mkey %#x ==> vf%d mkey %#x, and attached to CQN %#x\n",
			alias_mkey, vf_idx, target_pp->mkey[i], modify_mkey_cqn);
		pthread_mutex_lock(&lock);
		pp->alias_mkey_obj[vf_idx][i] = obj;
		pp->alias_mkey[vf_idx][i] = alias_mkey;
		pp->alias_mkey_mrbuf[vf_idx][i] = target_pp->mkey_mrbuf[i];
		pthread_mutex_unlock(&lock);

		// Destroy the old one
		if (old_alias_mkey_obj) {
			INFO("Wait for drain of old alias mkey %#x\n", old_alias_mkey);
			uint16_t pi = ppdv.qp.sq.cur_post & 0xffff;
			uint16_t ci = ppdv.qp.sq.wqe_ci;
			if (pi >= ci) { 
				// |-----------------ci------------pi-----------------|
				//       new_ci				new_ci
				INFO("----Sample: pi >= ci, pi: %#x, ci: %#x\n", pi, ci);
				while (true){
					uint16_t new_ci = ppdv.qp.sq.wqe_ci;
					if (new_ci < ci || pi <= new_ci) {
						INFO("----Drain of old alias mkey: new_ci < ci || pi <= new_ci.  new ci: %#x, ci: %#x, pi: %#x\n", new_ci, ci, pi);
						break;
					}
				}
			} else {
				// |-------pi---------------------------ci-----------|
				//                       new_ci 
				INFO("----Sample: pi < ci, pi: %#x, ci: %#x\n", pi, ci);
				while (true){
					uint16_t new_ci = ppdv.qp.sq.wqe_ci;
					if (pi <= new_ci && new_ci < ci) {
						INFO("Drain of old alias mkey: pi <= new_ci && new_ci < ci. new ci: %#x, ci: %#x, pi: %#x\n", new_ci, ci, pi);
						break;
					}
				}
			}

			INFO("Destory old alias mkey %#x\n", old_alias_mkey);
			mlx5dv_devx_obj_destroy(old_alias_mkey_obj);
		}
	}
	return 0;
}

static int get_next_cqe(struct pp_dv_cq *dvcq,
			struct mlx5_cqe64 **pcqe64)
{
	struct mlx5_cqe64 *cqe64;

	cqe64 = get_sw_cqe(dvcq, dvcq->cons_index);
	if (!cqe64)
		return CQ_EMPTY;

	++dvcq->cons_index;
	/*
         * Make sure we read CQ entry contents after we've checked the
         * ownership bit.
         */
	udma_from_device_barrier();

	*pcqe64 = cqe64;
	return CQ_OK;
}

static int poll_one_cq(struct pp_dv_cq *dvcq)
{
	struct mlx5_cqe64 *cqe64;
	int err;

	err = get_next_cqe(dvcq, &cqe64);
	if (err == CQ_EMPTY)
		return err;

	return parse_cqe(dvcq, cqe64);

	return 0;
}
#define snap_compiler_fence() asm volatile(""::: "memory")
#define priv_doca_memory_barrier() _mm_sfence()
int pp_dv_poll_cq(struct pp_dv_cq *dvcq, uint32_t ne)
{
	int npolled, err = 0;

	for (npolled = 0; npolled < ne; npolled ++) {
		err = poll_one_cq(dvcq);
		if (err != CQ_OK)
			break;
	}

	//udma_to_device_barrier();
	dvcq->db[MLX5_CQ_SET_CI] = htobe32(dvcq->cons_index & 0xffffff);
	return err == CQ_POLL_ERR ? err : npolled;
}

/* Ref.: rdma-core/providers/mlx5/qp.c::mlx5_post_wq_recv() */
int pp_dv_post_recv(const struct pp_context *ppc, struct pp_dv_qp *dvqp,
		    unsigned int num_post)
{
	int ind = dvqp->rq.head & (dvqp->rq.wqe_cnt - 1), nreq;
	struct mlx5_wqe_data_seg *seg;

	for (nreq = 0; nreq < num_post; nreq++) {
		//seg = get_wq_recv_wqe(dvqp, ind);
		seg = dvqp->buf + dvqp->rq.offset + (ind << dvqp->rq.wqe_shift);
		/* Signature is not supported */
		set_src_data(seg, nreq, ppc);
		//mlx5dv_set_data_seg(seg, ppc->mrbuflen, ppc->mr[nreq]->lkey,
		//		    (uint64_t)ppc->mrbuf[nreq]);
		ind = (ind + 1) & (dvqp->rq.wqe_cnt - 1);
	}

	dvqp->rq.head += nreq;
	/*
	 * Make sure that descriptors are written before
	 * doorbell record.
	 */
	udma_to_device_barrier();

	dvqp->db[MLX5_RCV_DBR] = htobe32(dvqp->rq.head & 0xffff);
	return 0;
}
