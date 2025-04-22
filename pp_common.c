#include "pp_common.h"
#include "mlx5_ifc.h"

static inline char *get_link_layer_str(int layer)
{
	if (layer == IBV_LINK_LAYER_INFINIBAND)
		return "infiniband";
	else if (layer == IBV_LINK_LAYER_ETHERNET)
		return "ethernet";
	else
		return "unspecified";

}

static int pp_open_ibvdevice(const char *ibv_devname, struct pp_context *ppctx)
{
	int i = 0, ret = 0;
	const char *devname;
	struct ibv_device **dev_list;

	dev_list = ibv_get_device_list(NULL);
	if (!dev_list) {
		ERR("ibv_get_device_list()");
		return -errno;
	}

	for (i = 0; dev_list[i] != NULL; i++) {
		devname = ibv_get_device_name(dev_list[i]);
		//printf("=DEBUG:%s:%d: %s, %s\n", __func__, __LINE__, dev, s_ibv_devname);
		if (strncmp(devname, ibv_devname, strlen(ibv_devname)) == 0) {
			INFO("Device found: %d/%s\n", i, devname);
			break;
		}
	}
	if (dev_list[i] == NULL) {
		ERR("Device not found: %d/%s\n", i, ibv_devname);
		return -1;
	}

	ppctx->dev_list = dev_list;
	ppctx->ibctx = ibv_open_device(dev_list[i]);
	if (ppctx->ibctx == NULL) {
		ERR("ibv_open_device(%s), i=%d", ibv_devname, i);
		return errno;
	}

	//INFO("show port list\n");
	//for (int i = 1; i < 10; i++) {
	//	ret = ibv_query_port(ppctx->ibctx, i, &ppctx->port_attr);
	//	if (ret) {
	//		perror("ibv_query_port");
	//		return ret;
	//	}
	//	INFO("ibdev %s port %d port_state %d (expect %d) phy_state %d\n", ibv_devname, ppctx->port_num,
	//	     ppctx->port_attr.state, IBV_PORT_ACTIVE, ppctx->port_attr.phys_state);
	//}
	ppctx->port_num = PORT_NUM;
	do {
		ret = ibv_query_port(ppctx->ibctx, PORT_NUM, &ppctx->port_attr);
		if (ret) {
			perror("ibv_query_port");
			return ret;
		}
		INFO("ibdev %s port %d port_state %d (expect %d) phy_state %d\n", ibv_devname, ppctx->port_num,
		     ppctx->port_attr.state, IBV_PORT_ACTIVE, ppctx->port_attr.phys_state);

		if (ppctx->port_attr.state == IBV_PORT_ACTIVE)
			break;
		sleep(1);
	} while (1);

	INFO("ibdev %s port %d lid %d state %d, mtu max %d active %d, link_layer %d(%s) phy_state %d speed %d\n",
	     ibv_devname, ppctx->port_num, ppctx->port_attr.lid, ppctx->port_attr.state,
	     ppctx->port_attr.max_mtu, ppctx->port_attr.active_mtu,
	     ppctx->port_attr.link_layer, get_link_layer_str(ppctx->port_attr.link_layer),
	     ppctx->port_attr.phys_state, ppctx->port_attr.active_speed);

	/*
        if (ppctx->port_attr.link_layer != IBV_LINK_LAYER_ETHERNET) {
		server_sgid_idx = 0;
		client_sgid_idx = 0;
	}
	*/
	return 0;
}

static void pp_close_ibvdevice(struct ibv_context *ibctx)
{
	int err = ibv_close_device(ibctx);
	if (err)
		perror("mz_close_ibvdevice");
}

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

static uint32_t get_pdn(struct ibv_pd *pd)
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

static int dr_devx_query_gvmi(struct ibv_context *ctx, uint16_t *gvmi)
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

int pp_ctx_init(struct pp_context *pp, const char *ibv_devname, int use_vfio, const char *vfio_pci_name)
{
	struct mlx5dv_vfio_context_attr vfio_ctx_attr = {
		.pci_name = vfio_pci_name,
		.flags = MLX5DV_VFIO_CTX_FLAGS_INIT_LINK_DOWN,
		.comp_mask = 0,
	};
	int ret, i;

	pp->cap.max_send_wr = PP_MAX_WR * 1024;
	pp->cap.max_recv_wr = PP_MAX_WR;
	pp->cap.max_send_sge = 1;
	pp->cap.max_recv_sge = 1;
	pp->cap.max_inline_data = 64;

	if (use_vfio) {
		struct ibv_device *ibdev;

		pp->dev_list = mlx5dv_get_vfio_device_list(&vfio_ctx_attr);
		if (!pp->dev_list) {
			ERR("mlx5dv_get_vfio_device_list returns NULL\n");
			return errno;
		}
		ibdev = pp->dev_list[0];
		pp->ibctx = ibv_open_device(ibdev);
		if (!pp->ibctx) {
			ERR("ibv_open_device(%s) failed: %d\n", vfio_pci_name, errno);
			return errno;
		}
		pp->port_num = PORT_NUM;
	} else {
		if (pp->ibctx) {
			pp_close_ibvdevice(pp->ibctx);
			pp->ibctx = NULL;
		}
		ret = pp_open_ibvdevice(ibv_devname, pp);
		if (ret)
			return ret;
	}

	pp->pd = ibv_alloc_pd(pp->ibctx);
	if (!pp->pd) {
		ERR("ibv_alloc_pd() failed\n");
		ret = errno;
		goto fail_alloc_pd;
	}
	
	pp->pdn = get_pdn(pp->pd);
	INFO("pdn: %#x\n", pp->pdn);

	uint16_t vhca_id;
	if (dr_devx_query_gvmi(pp->ibctx, &vhca_id)) {
		ERR("failed to get vhca_id");
		goto fail_alloc_pd;
		return 2;
	}
	pp->vhca_id = vhca_id;
	INFO("vhca_id: %#x\n", pp->vhca_id);

	pp->mrbuflen = PP_DATA_BUF_LEN;
	for (i = 0; i < PP_MAX_WR; i++) {
		if (pp->mem_region_type == MEM_REGION_TYPE_NONE) {
			continue;
		}

		pp->mrbuf[i] = memalign(sysconf(_SC_PAGESIZE), pp->mrbuflen);
		if (!pp->mrbuf[i]) {
			ERR("%d: memalign(0x%lx) failed\n", i, pp->mrbuflen);
			ret = errno;
			goto fail_memalign;
		}

		if (pp->mem_region_type == MEM_REGION_TYPE_MR){
			pp->mr[i] = ibv_reg_mr(pp->pd, pp->mrbuf[i], pp->mrbuflen,
					IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_READ | IBV_ACCESS_REMOTE_WRITE);
			if (!pp->mr[i]) {
				ERR("%d: ibv_reg_mr() failed\n", i);
				ret = errno;
				goto fail_reg_mr;
			}
			INFO("%d mr mkey %#x\n", i, pp->mr[i]->lkey);
		} else if (pp->mem_region_type == MEM_REGION_TYPE_DEVX) {
			pp->mkey_mrbuf[i] = pp->mrbuf[i];
			pp->mrbuf[i] = NULL;
			pp->umem_obj[i] = mlx5dv_devx_umem_reg(pp->ibctx, pp->mkey_mrbuf[i], pp->mrbuflen, IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_READ | IBV_ACCESS_REMOTE_WRITE);
			if (!pp->umem_obj) {
				ERR("umem reg failed port %zu", pp->mrbuflen);
				goto fail_memalign;
			}

			uint32_t mkey = 0;
			struct mlx5dv_devx_obj *obj = buf_mkey_obj_create(pp->ibctx, pp->pdn, pp->umem_obj[i]->umem_id, pp->mkey_mrbuf[i], pp->mrbuflen, &mkey);
			if (!obj) {
				ERR("mkey create failed");
				goto fail_memalign;
			}

			pp->mkey_obj[i] = obj;
			pp->mkey[i] = mkey;
			INFO("%d devx mkey %#x\n", i, pp->mkey[i]);
		} else {
			ERR("Incorrect mem region type %d\n", pp->mem_region_type);
		}
	}

	if (use_vfio)
		INFO("VFIO open(%s) succeeds, flags 0x%x\n\n", vfio_pci_name, vfio_ctx_attr.flags);
	else
		INFO("Initialization succeeds(regular)\n\n");


	return 0;

fail_reg_mr:
	for (i = 0; i < PP_MAX_WR; i++)
		if (pp->mr[i])
			ibv_dereg_mr(pp->mr[i]);
fail_memalign:
	for (i = 0; i < PP_MAX_WR; i++)
		free(pp->mrbuf[i]);
	ibv_dealloc_pd(pp->pd);
fail_alloc_pd:
	pp_close_ibvdevice(pp->ibctx);

	return ret;
}

void pp_ctx_cleanup(struct pp_context *pp)
{
	int i;

	for (i = 0; i < PP_MAX_WR; i++) {
		if (MEM_REGION_TYPE_MR == pp->mem_region_type) {
			ibv_dereg_mr(pp->mr[i]);
			free(pp->mrbuf[i]);
			pp->mr[i] = NULL;
			pp->mrbuf[i] = NULL;
		} else if (MEM_REGION_TYPE_DEVX == pp->mem_region_type) {
			mlx5dv_devx_obj_destroy(pp->mkey_obj[i]);
			mlx5dv_devx_umem_dereg(pp->umem_obj[i]);
			free(pp->mkey_mrbuf[i]);
			pp->mkey_obj[i] = NULL;
			pp->mkey[i] = 0;
			pp->umem_obj[i] = NULL;
			pp->mkey_mrbuf[i] = NULL;
		} else if (MEM_REGION_TYPE_ALIAS_VF0 == pp->mem_region_type ||
			   MEM_REGION_TYPE_ALIAS_VF1 == pp->mem_region_type) {
			mlx5dv_devx_obj_destroy(pp->alias_mkey_obj[0][i]);
			mlx5dv_devx_obj_destroy(pp->alias_mkey_obj[1][i]);
			pp->alias_mkey_obj[0][i] = NULL;
			pp->alias_mkey[0][i] = 0;
			pp->alias_mkey_mrbuf[0][i] = NULL;
			pp->alias_mkey_obj[1][i] = NULL;
			pp->alias_mkey[1][i] = 0;
			pp->alias_mkey_mrbuf[1][i] = NULL;
		}
	}
	ibv_dealloc_pd(pp->pd);
	pp->pd = NULL;
	pp_close_ibvdevice(pp->ibctx);
	pp->ibctx = NULL;
}

static void print_gid(struct pp_context *ppc, unsigned char *p)
{
	if (ppc->port_attr.link_layer != IBV_LINK_LAYER_ETHERNET)
		return;

	printf("                                    gid %02x:%02x:%02x:%02x:%02x:%02x:%02x:%02x:%02x:%02x:%02x:%02x:%02x:%02x:%02x:%02x\n",
	       p[0], p[1], p[2], p[3], p[4], p[5], p[6], p[7],
	       p[8], p[9], p[10], p[11], p[12], p[13], p[14], p[15]);
}

extern int sock_client(const char *server_ip, char *sendbuf, int send_buflen,
		       char *recvbuf, int recv_buflen);
extern int sock_server(char *sendbuf, int send_buflen, char *recvbuf, int recv_buflen);

void get_mkey_buf(struct pp_context *ppc, int i, uint32_t *mkey, unsigned char **buf)
{
	if (ppc->mem_region_type == MEM_REGION_TYPE_MR) {
		*mkey = ppc->mr[i]->lkey;
		*buf = ppc->mrbuf[i];
	} else if (ppc->mem_region_type == MEM_REGION_TYPE_DEVX) {
		*mkey = ppc->mkey[i];
		*buf = ppc->mkey_mrbuf[i];
	} else if (ppc->mem_region_type == MEM_REGION_TYPE_ALIAS_VF0) {
		*mkey = ppc->alias_mkey[0][i];
		*buf = ppc->alias_mkey_mrbuf[0][i];
	} else if (ppc->mem_region_type == MEM_REGION_TYPE_ALIAS_VF1) {
		*mkey = ppc->alias_mkey[1][i];
		*buf = ppc->alias_mkey_mrbuf[1][i];
	}
}

/* %sip NULL means local is server, otherwise local is client */
int pp_exchange_info(struct pp_context *ppc, int my_sgid_idx,
		     int my_qp_num, uint32_t my_psn,
		     struct pp_exchange_info *remote, const char *sip)
{
	char sendbuf[4096] = {}, recvbuf[4096] = {};
	struct pp_exchange_info *local = (struct pp_exchange_info *)sendbuf;
	struct pp_exchange_info *r = (struct pp_exchange_info *)recvbuf;
	unsigned char *p;
	int ret, i;

	if (!ppc->port_num) {
		ERR("pp_context isn't initialized: pp->port_num is 0\n");
		return -1;
	}

	if (ppc->port_attr.link_layer == IBV_LINK_LAYER_ETHERNET) {
		ret = ibv_query_gid(ppc->ibctx, ppc->port_num,
				    my_sgid_idx, &local->gid);
		if (ret) {
			ERR("ibv_query_gid failed %d\n", ret);
			return ret;
		}
	} else {
		local->lid = htobe32(ppc->port_attr.lid);
	}

	local->qpn = htobe32(my_qp_num);
	local->psn = htobe32(my_psn);

	uint32_t  mkey = 0;
	unsigned char *buf = NULL;
	for (i = 0; i < PP_MAX_WR; i++) {
		get_mkey_buf(ppc, i, &mkey, &buf);
		local->addr[i] = (void *)htobe64((uint64_t)buf);
		local->mrkey[i] = htobe32(mkey);
	}
	p = local->gid.raw;
	INFO("Local(%s): port_num %d, lid %d, psn 0x%x, qpn 0x%x(%d), addr %p, mrkey 0x%x\n",
	     sip ? "Client" : "Server", ppc->port_num, ppc->port_attr.lid, my_psn,
	     my_qp_num, my_qp_num, buf, mkey);
	print_gid(ppc, p);

	if (sip)
		ret = sock_client(sip, sendbuf, sizeof(*local),
				  recvbuf, sizeof(recvbuf));
	else
		ret = sock_server(sendbuf, sizeof(*local),
				  recvbuf, sizeof(recvbuf));
	if (ret) {
		ERR("socket failed %d, server_ip %s\n", ret, sip ? sip : "");
		return ret;
	}

	remote->lid = be32toh(r->lid);
	remote->qpn = be32toh(r->qpn);
	remote->psn = be32toh(r->psn);
	for (i = 0; i < PP_MAX_WR; i++) {
		remote->addr[i] = (void *)be64toh((uint64_t)r->addr[i]);
		remote->mrkey[i] = be32toh(r->mrkey[i]);
	}
	memcpy(&remote->gid, &r->gid, sizeof(remote->gid));
	p = remote->gid.raw;
	INFO("Remote(%s): lid %d, psn 0x%x, qpn 0x%x(%d), addr %p, mrkey 0x%x\n",
	     sip ? "Server" : "Client", remote->lid, remote->psn,
	     remote->qpn, remote->qpn, remote->addr[PP_MAX_WR - 1], remote->mrkey[PP_MAX_WR - 1]);
	print_gid(ppc, p);
	printf("\n");

	return 0;
}

/* To dump a long string like this: "0: 0BCDEFGHIJKLMNOP ... nopqrstuvwxyABC" */
void dump_msg_short(int index, struct pp_context *ppc)
{
	uint32_t mkey;
	unsigned char *buf;
	get_mkey_buf(ppc, index, &mkey, &buf);

	buf[ppc->mrbuflen - 1] = '\0';
	if (ppc->mrbuflen <= 32) {
		printf("    %2d: %s\n", index, buf);
	} else {
		buf[16] = '\0';
		printf("    %2d (len = 0x%lx): %s...%s\n", index, ppc->mrbuflen,
		       buf, buf + ppc->mrbuflen - 16);
	}
}
