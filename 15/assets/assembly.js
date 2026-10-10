/* =====================================================================
   15/ — 柜体装配视图数据（爆炸图 / 一次接线图的共用地基）
   ---------------------------------------------------------------------
   ★★ 本文件是**派生产物**，由 `15/scripts/gen_assembly.py` 生成，请勿手改。
      源 = `docs/规划/部件测点映射表.csv`（78 行 × 9 列）。
      改动请走生成器：  python 15/scripts/gen_assembly.py
      （加 --check 可只比对不写盘，用于 CI / 自检）

   字段语义
     src              该部位 / 部件在 CSV 里的**原值**。
                      15/tests/selftest.js 的 J 段用它做精确 join，
                      逐点比对「测点（现场口径）/ 设备点名 / 单位」三列 ——
                      这是"界面结构没与现场点表脱钩"唯一的自动拦截点。
     id               界面内部标识（点部件、跨渲染记忆选中态都用它）
     name             显示名（带序号、带"站级"等页面口径，与 src 故意不同）
     dev/key/unit/mul 运行时绑定。dev=null 表示**设计上就没有实时数据源**
                      （不是"暂时没数据"）—— 界面必须显式显示"无实时源"，
                      把它涂成"正常"等于谎报现场这个部件在传数据。
     points[]         该部件的全部测点定义（点名 / 设备侧点名 / 单位）

   当前 14 个部件里只有 5 个能绑定实时值（cluster/cell/conv/meter/pv），
   因为 `/api/realtime` 只有 DEV-METER-01 / DEV-PV-01 / DEV-LOAD-01 /
   DEV-PCS-01 / DEV-BMS-01 五台设备。其余 9 个部件的点要走 `13/` 的
   Modbus 接入才有实时值 —— 这是事实，不是待办，界面按事实呈现。
   ===================================================================== */

const ASSEMBLY = [
    {
      id: 'bat',
      name: '① 电池舱',
      src: '柜内·电池舱',
      parts: [
        {
          id: 'cluster',
          name: '电池簇',
          src: '电池簇',
          n: 8,
          dev: 'DEV-BMS-01',
          key: 'soc',
          unit: '%',
          mul: 100,
          points: [
              { n: '簇电压', d: 'DP_BMS_CLUSTER_U', u: 'V' },
              { n: '簇电流', d: 'DP_BMS_CLUSTER_I', u: 'A' },
              { n: '簇温度', d: 'DP_BMS_CLUSTER_T', u: '°C' },
              { n: 'SOC', d: 'DP_BMS_SOC', u: '0..1' },
              { n: 'SOH', d: 'DP_BMS_SOH', u: '0..1' },
              { n: '剩余可充电量', d: 'DP_BMS_RM_CHG_KWH', u: 'kWh' },
              { n: '剩余可放电量', d: 'DP_BMS_RM_DIS_KWH', u: 'kWh' },
              { n: '循环次数', d: 'DP_BMS_CYCLE_COUNT', u: '次' }
          ]
        },
        {
          id: 'cell',
          name: '电芯（模组内单体）',
          src: '电芯（模组内单体）',
          n: 8,
          dev: 'DEV-BMS-01',
          key: 'temperature_c',
          unit: '°C',
          mul: 1,
          points: [
              { n: '单体最高电压', d: 'DP_BMS_CELL_V_MAX', u: 'V' },
              { n: '最高电压单体编号', d: 'DP_BMS_CELL_V_MAX_IDX', u: '#' },
              { n: '单体最低电压', d: 'DP_BMS_CELL_V_MIN', u: 'V' },
              { n: '最低电压单体编号', d: 'DP_BMS_CELL_V_MIN_IDX', u: '#' },
              { n: '单体压差', d: 'DP_BMS_CELL_V_DIFF', u: 'V' },
              { n: '单体最高温度', d: 'DP_BMS_T_MAX', u: '°C' },
              { n: '最高温度点位编号', d: 'DP_BMS_T_MAX_IDX', u: '#' },
              { n: '单体最低温度', d: 'DP_BMS_T_MIN', u: '°C' }
          ]
        },
        {
          id: 'insul',
          name: '绝缘监测',
          src: '绝缘监测',
          n: 2,
          dev: null,
          key: null,
          unit: '',
          mul: 1,
          points: [
              { n: '绝缘电阻', d: 'DP_BMS_INSUL_R', u: 'kΩ' },
              { n: '漏电流', d: 'DP_BMS_LEAK_I', u: 'mA' }
          ]
        },
        {
          id: 'contactor',
          name: '直流接触器',
          src: '直流接触器',
          n: 4,
          dev: null,
          key: null,
          unit: '',
          mul: 1,
          points: [
              { n: '主正接触器状态', d: 'DP_BMS_MAIN_POS', u: '0/1' },
              { n: '主负接触器状态', d: 'DP_BMS_MAIN_NEG', u: '0/1' },
              { n: '预充继电器状态', d: 'STA.BMS.PRECHARGE', u: '0/1' },
              { n: '充电状态位', d: 'DP_BMS_CHARGING', u: '0/1' }
          ]
        },
        {
          id: 'bms',
          name: 'BMS 主控',
          src: 'BMS 主控',
          n: 10,
          dev: null,
          key: null,
          unit: '',
          mul: 1,
          points: [
              { n: '通信正常', d: 'DP_BMS_COMM_OK', u: '0/1' },
              { n: '心跳计数', d: 'DP_BMS_HEARTBEAT', u: '次' },
              { n: '数据有效位', d: 'DP_BMS_DATA_VALID', u: '0/1' },
              { n: '禁止充电', d: 'DP_BMS_CHG_FORBID', u: '0/1' },
              { n: '禁止放电', d: 'DP_BMS_DIS_FORBID', u: '0/1' },
              { n: '禁充放原因码', d: 'DP_BMS_FORBID_REASON', u: '码' },
              { n: '允许充电功率', d: 'DP_BMS_CHG_LIMIT_KW', u: 'kW' },
              { n: '允许放电功率', d: 'DP_BMS_DIS_LIMIT_KW', u: 'kW' },
              { n: '告警级别', d: 'DP_BMS_ALM_LEVEL', u: '级' },
              { n: '告警码', d: 'DP_BMS_ALM_CODE', u: '码' }
          ]
        }
      ]
    },
    {
      id: 'pcs',
      name: '② PCS 舱',
      src: '柜内·PCS舱',
      parts: [
        {
          id: 'conv',
          name: 'PCS 变流器',
          src: 'PCS 变流器',
          n: 14,
          dev: 'DEV-PCS-01',
          key: 'power_kw',
          unit: 'kW',
          mul: 1,
          points: [
              { n: '有功功率', d: 'DP_PCS_P_ACT', u: 'kW' },
              { n: '无功功率', d: 'DP_PCS_Q_ACT', u: 'kvar' },
              { n: '直流侧电压', d: 'DP_PCS_UDC', u: 'V' },
              { n: '直流侧电流', d: 'DP_PCS_IDC', u: 'A' },
              { n: '交流侧三相电压', d: 'DP_PCS_U_A/B/C', u: 'V' },
              { n: '交流侧三相电流', d: 'DP_PCS_I_A/B/C', u: 'A' },
              { n: '运行模式', d: 'DP_PCS_MODE', u: '码' },
              { n: '故障位', d: 'DP_PCS_FAULT', u: '0/1' },
              { n: '故障码', d: 'DP_PCS_FAULT_CODE', u: '码' },
              { n: '通信正常', d: 'DP_PCS_COMM_OK', u: '0/1' },
              { n: '限功率原因', d: 'DP_PCS_LIMIT_REASON', u: '码' },
              { n: '日发电量 / 累计发电量', d: 'DP_PCS_E_DAY_KWH / E_TOTAL_KWH', u: 'kWh' },
              { n: '运行小时', d: 'DP_PCS_RUN_HOURS', u: 'h' },
              { n: 'IGBT / 模块温度', d: 'MEAS.PCS.T_MODULE', u: '°C' }
          ]
        },
        {
          id: 'cmdp',
          name: 'PCS 指令落点',
          src: 'PCS 指令落点',
          n: 3,
          dev: null,
          key: null,
          unit: '',
          mul: 1,
          points: [
              { n: '有功设定', d: 'DP_PCS_CMD_P_SET', u: 'kW' },
              { n: '允许上界 / 下界', d: 'DP_PCS_CMD_P_UPPER / P_LOWER', u: 'kW' },
              { n: '启停遥控', d: 'DP_PCS_CMD_ONOFF', u: '0/1' }
          ]
        }
      ]
    },
    {
      id: 'aux',
      name: '③ 辅助舱',
      src: '柜内·辅助',
      parts: [
        {
          id: 'fire',
          name: '消防主机',
          src: '消防主机',
          n: 5,
          dev: null,
          key: null,
          unit: '',
          mul: 1,
          points: [
              { n: '主机状态（正常/火警/故障）', d: 'STA.FIRE.HOST_STATE', u: '0/1/2' },
              { n: '烟感报警', d: 'ALM.FIRE.SMOKE', u: '0/1' },
              { n: '温感报警', d: 'ALM.FIRE.HEAT', u: '0/1' },
              { n: '喷洒 / 释放信号', d: 'STA.FIRE.RELEASED', u: '0/1' },
              { n: '灭火装置压力', d: 'MEAS.FIRE.PRESSURE', u: 'MPa' }
          ]
        },
        {
          id: 'hvac',
          name: '空调 / 液冷机组',
          src: '空调 / 液冷机组',
          n: 7,
          dev: null,
          key: null,
          unit: '',
          mul: 1,
          points: [
              { n: '柜内环境温度', d: 'MEAS.HVAC.T_AMB', u: '°C' },
              { n: '回风温度', d: 'MEAS.HVAC.T_RETURN', u: '°C' },
              { n: '进 / 出水温（液冷）', d: 'MEAS.HVAC.T_WATER', u: '°C' },
              { n: '冷却液流量', d: 'MEAS.HVAC.FLOW', u: 'L/min' },
              { n: '水泵运行状态', d: 'STA.HVAC.PUMP_ON', u: '0/1' },
              { n: '启停（遥调）', d: 'CMD.HVAC.ONOFF', u: '0/1' },
              { n: '目标温度（遥调）', d: 'CMD.HVAC.T_SET', u: '°C' }
          ]
        },
        {
          id: 'sec',
          name: '门禁 / 安防',
          src: '门禁 / 安防',
          n: 2,
          dev: null,
          key: null,
          unit: '',
          mul: 1,
          points: [
              { n: '柜门磁状态', d: 'STA.SEC.DOOR_OPEN', u: '0/1' },
              { n: '水浸报警', d: 'ALM.SEC.WATER', u: '0/1' }
          ]
        },
        {
          id: 'sw',
          name: '配电 / 开关',
          src: '配电 / 开关',
          n: 3,
          dev: null,
          key: null,
          unit: '',
          mul: 1,
          points: [
              { n: '交流侧断路器分合', d: 'STA.SWITCH.AC_BREAKER', u: '0/1' },
              { n: '脱扣原因', d: 'STA.SWITCH.TRIP_REASON', u: '码' },
              { n: '急停按钮状态', d: 'STA.SWITCH.ESTOP', u: '0/1' }
          ]
        }
      ]
    },
    {
      id: 'sta',
      name: '④ 站级·站内一次',
      src: '站内·一次',
      parts: [
        {
          id: 'meter',
          name: '关口电表',
          src: '关口电表',
          n: 9,
          dev: 'DEV-METER-01',
          key: 'power_kw',
          unit: 'kW',
          mul: 1,
          points: [
              { n: '关口功率', d: 'DP_METER_P_TOTAL', u: 'kW' },
              { n: '三相电压 / 电流', d: 'DP_METER_U_A..C / I_A..C', u: 'V/A' },
              { n: '频率', d: 'DP_METER_FREQ', u: 'Hz' },
              { n: '正 / 反向电能', d: 'DP_METER_EP_FWD / EP_REV', u: 'kWh' },
              { n: '当前需量 / 峰值需量', d: 'DP_METER_DEMAND_NOW / PEAK', u: 'kW' },
              { n: '通信正常', d: 'DP_METER_COMM_OK', u: '0/1' },
              { n: '分路功率（负荷 / 光伏）', d: 'DP_METER_P_LOAD / P_PV', u: 'kW' },
              { n: '变压器容量（一次侧配置）', d: 'DP_METER_TR_KVA', u: 'kVA' },
              { n: '契约需量（配置）', d: 'DP_METER_D_TARGET', u: 'kW' }
          ]
        },
        {
          id: 'tr',
          name: '变压器',
          src: '变压器',
          n: 2,
          dev: null,
          key: null,
          unit: '',
          mul: 1,
          points: [
              { n: '绕组温度 / 油温', d: 'MEAS.METER.TR_TEMP', u: '°C' },
              { n: '负载率', d: 'MEAS.METER.TR_LOAD_RATE', u: '%' }
          ]
        },
        {
          id: 'pv',
          name: '光伏逆变器',
          src: '光伏逆变器',
          n: 1,
          dev: 'DEV-PV-01',
          key: 'power_kw',
          unit: 'kW',
          mul: 1,
          points: [
              { n: '光伏出力', d: 'DP_METER_P_PV', u: 'kW' }
          ]
        }
      ]
    }
  ];
