(ns editor.game-object
  (:require [clojure.java.io :as io]
            [editor.protobuf :as protobuf]
            [dynamo.graph :as g]
            [editor.graph-util :as gu]
            [editor.core :as core]
            [editor.dialogs :as dialogs]
            [editor.geom :as geom]
            [editor.handler :as handler]
            [editor.math :as math]
            [editor.defold-project :as project]
            [editor.scene :as scene]
            [editor.types :as types]
            [editor.sound :as sound]
            [editor.resource :as resource]
            [editor.workspace :as workspace]
            [editor.properties :as properties]
            [editor.validation :as validation]
            [editor.outline :as outline])
  (:import [com.dynamo.gameobject.proto GameObject$PrototypeDesc]
           [com.dynamo.graphics.proto Graphics$Cubemap Graphics$TextureImage Graphics$TextureImage$Image Graphics$TextureImage$Type]
           [com.dynamo.sound.proto Sound$SoundDesc]
           [com.dynamo.proto DdfMath$Point3 DdfMath$Quat]
           [com.jogamp.opengl.util.awt TextRenderer]
           [editor.types Region Animation Camera Image TexturePacking Rect EngineFormatTexture AABB TextureSetAnimationFrame TextureSetAnimation TextureSet]
           [java.awt.image BufferedImage]
           [java.io PushbackReader]
           [javax.media.opengl GL GL2 GLContext GLDrawableFactory]
           [javax.media.opengl.glu GLU]
           [javax.vecmath Matrix4d Point3d Quat4d Vector3d]
           [org.apache.commons.io FilenameUtils]))

(set! *warn-on-reflection* true)

(def game-object-icon "icons/32/Icons_06-Game-object.png")
(def unknown-icon "icons/32/Icons_29-AT-Unkown.png") ; spelling...

(defn- gen-ref-ddf
  ([id position ^Quat4d rotation-q4 path]
    (gen-ref-ddf id position rotation-q4 path {}))
  ([id position ^Quat4d rotation-q4 path source-properties]
    {:id id
     :position position
     :rotation (math/vecmath->clj rotation-q4)
     :component (resource/resource->proj-path path)
     :properties (->> source-properties
                   (filter (fn [[key p]] (contains? p :original-value)))
                   (mapv (fn [[key p]]
                           {:name (name key)
                            :type (:go-prop-type p)
                            :value (:value p)})))}))

(defn- gen-embed-ddf [id position ^Quat4d rotation-q4 save-data]
  {:id id
   :type (or (and (:resource save-data) (:ext (resource/resource-type (:resource save-data))))
             "unknown")
   :position position
   :rotation (math/vecmath->clj rotation-q4)
   :data (or (:content save-data) "")})

(def sound-exts (into #{} (map :ext sound/sound-defs)))

(defn- wrap-if-raw-sound [_node-id target]
  (let [source-path (resource/proj-path (:resource (:resource target)))
        ext (FilenameUtils/getExtension source-path)]
    (if (sound-exts ext)
      (let [workspace (project/workspace (project/get-project _node-id))
            res-type  (workspace/get-resource-type workspace "sound")
            pb        {:sound source-path}
            target    {:node-id  _node-id
                       :resource (workspace/make-build-resource (resource/make-memory-resource workspace res-type (protobuf/map->str Sound$SoundDesc pb)))
                       :build-fn (fn [self basis resource dep-resources user-data]
                                   (let [pb (:pb user-data)
                                         pb (assoc pb :sound (resource/proj-path (second (first dep-resources))))]
                                     {:resource resource :content (protobuf/map->bytes Sound$SoundDesc pb)}))
                       :deps     [target]}]
        target)
      target)))

(defn- source-properties-subst [err]
  {:properties {}
   :display-order []})

(defn- source-outline-subst [err]
  ;; TODO: embed error
  {:node-id 0
   :icon ""
   :label ""})

(g/defnode ComponentNode
  (inherits scene/SceneNode)
  (inherits outline/OutlineNode)

  (property id g/Str)

  ;; TODO - remove
  (property properties g/Any)

  (display-order [:id :path scene/SceneNode])

  #_(input source-id g/NodeID)
  #_(input source-resource (g/protocol resource/Resource))
  (input source-properties g/Properties)
  #_(input user-properties g/Any :substitute source-properties-subst)
  #_(input project-id g/NodeID)
  (input scene g/Any)
  (input build-targets g/Any)

  (input source-outline outline/OutlineData :substitute source-outline-subst)
  (output source-outline outline/OutlineData (g/fnk [source-outline] source-outline))

  (output node-outline outline/OutlineData :cached
    (g/fnk [_node-id node-outline-label id source-outline]
      (let [source-outline (or source-outline {:icon unknown-icon})]
        (assoc source-outline :node-id _node-id :label node-outline-label))))
  (output node-outline-label g/Str (g/fnk [id] id))
  (output ddf-message g/Any :abstract)
  (output scene g/Any :cached (g/fnk [_node-id transform scene]
                                     (-> scene
                                       (assoc :node-id _node-id
                                              :transform transform
                                              :aabb (geom/aabb-transform (geom/aabb-incorporate (get scene :aabb (geom/null-aabb)) 0 0 0) transform))
                                       (update :node-path (partial cons _node-id)))))
  (output build-targets g/Any :cached (g/fnk [_node-id build-targets ddf-message transform]
                                             (if-let [target (first build-targets)]
                                               (let [target (wrap-if-raw-sound _node-id target)]
                                                 [(assoc target :instance-data {:resource (:resource target)
                                                                                :instance-msg ddf-message
                                                                                :transform transform})])
                                               []))))

(g/defnode EmbeddedComponent
  (inherits ComponentNode)

  (input save-data g/Any :cascade-delete)
  (output ddf-message g/Any :cached (g/fnk [id position ^Quat4d rotation-q4 save-data]
                                           (gen-embed-ddf id position rotation-q4 save-data)))
  (output _properties g/Properties :cached (g/fnk [_declared-properties source-properties]
                                                  (merge-with into _declared-properties source-properties))))

(g/defnode ReferencedComponent
  (inherits ComponentNode)

  (property resource-type g/Any
            (dynamic visible (g/always false)))
  (property path (g/protocol resource/Resource)
            (dynamic edit-type (g/fnk [resource-type]
                                      {:type (g/protocol resource/Resource)
                                       :ext (:ext resource-type)}))
            (value (gu/passthrough source-resource))
            (set (project/gen-resource-setter [#_[:_node-id :source-id]
                                               [:resource :source-resource]
                                               [:node-outline :source-outline]
                                               [:user-properties :user-properties]
                                               [:scene :scene]
                                               [:build-targets :build-targets]]))
            (validate (g/fnk [path]
                        (when (nil? path)
                          (g/error-warning "Missing component")))))

  (input source-resource (g/protocol resource/Resource))
  (output node-outline-label g/Str :cached (g/fnk [id source-resource]
                                                  (format "%s - %s" id (resource/resource->proj-path source-resource))))
  (output ddf-message g/Any :cached (g/fnk [id position ^Quat4d rotation-q4 source-resource]
                                           (gen-ref-ddf id position rotation-q4 source-resource))))

(g/defnode OverriddenComponent
  (inherits ReferencedComponent)

  (property path g/Any
            (dynamic edit-type (g/fnk [resource-type]
                                      {:type (g/protocol resource/Resource)
                                       :ext (:ext resource-type)
                                       :to-type (fn [v] (:resource v))
                                       :from-type (fn [r] {:resource r :overrides {}})}))
            (value (g/fnk [source-resource property-overrides]
                          {:resource source-resource
                           :overrides property-overrides}))
            (set (fn [basis self _ new-value]
                   (let [project (project/get-project self)]
                     (concat
                      (if-let [old-source (g/node-value self :source-id :basis basis)]
                        (g/delete-node old-source)
                        [])
                      (if (and new-value (:resource new-value))
                        (project/connect-resource-node project (:resource new-value) self []
                                                       (fn [comp-node]
                                                         (let [override (g/override basis comp-node {:traverse? (constantly false)})
                                                               id-mapping (:id-mapping override)
                                                               or-node (get id-mapping comp-node)]
                                                           (concat
                                                             (:tx-data override)
                                                             (let [outputs (g/output-labels (g/node-type* comp-node))]
                                                               (for [[from to] [[:_node-id :source-id]
                                                                                [:resource :source-resource]
                                                                                [:node-outline :source-outline]
                                                                                [:_properties :source-properties]
                                                                                [:scene :scene]
                                                                                [:build-targets :build-targets]]
                                                                     :when (contains? outputs from)]
                                                                 (g/connect or-node from self to)))
                                                             (for [[label value] (:overrides new-value)]
                                                               (g/set-property or-node label value))))))
                          [])))))
            (validate (g/fnk [path]
                        (when (nil? (:resource path))
                          (g/error-warning "Missing component")))))
  (input source-id g/NodeID :cascade-delete)
  (output property-overrides g/Any :cached
          (g/fnk [source-properties]
                 (into {}
                       (map (fn [[key p]] [key (:value p)])
                            (filter (fn [[_ p]] (contains? p :original-value))
                                    (:properties source-properties))))))
  (output ddf-message g/Any :cached (g/fnk [id position ^Quat4d rotation-q4 source-resource source-properties]
                                           (gen-ref-ddf id position rotation-q4 source-resource source-properties)))
  ;; TODO - cache it, not possible now because of dynamic prop invalidation bug
  (output _properties g/Properties #_:cached (g/fnk [_node-id _declared-properties source-properties]
                                                    (merge-with into _declared-properties source-properties))))

(g/defnk produce-proto-msg [ref-ddf embed-ddf]
  {:components ref-ddf
   :embedded-components embed-ddf})

(g/defnk produce-save-data [resource proto-msg]
  {:resource resource
   :content (protobuf/map->str GameObject$PrototypeDesc proto-msg)})

(defn- externalize [inst-data resources]
  (map (fn [data]
         (let [{:keys [resource instance-msg transform]} data
               resource (get resources resource)
               instance-msg (dissoc instance-msg :type :data)]
           (merge instance-msg
                  {:component (resource/proj-path resource)})))
       inst-data))

(defn- build-props [component]
  (let [properties (mapv #(assoc % :value (properties/str->go-prop (:value %) (:type %))) (:properties component))]
    (assoc component :property-decls (properties/properties->decls properties))))

(defn- build-game-object [self basis resource dep-resources user-data]
  (let [instance-msgs (externalize (:instance-data user-data) dep-resources)
        instance-msgs (mapv build-props instance-msgs)
        msg {:components instance-msgs}]
    {:resource resource :content (protobuf/map->bytes GameObject$PrototypeDesc msg)}))

(g/defnk produce-build-targets [_node-id resource proto-msg dep-build-targets]
  [{:node-id _node-id
    :resource (workspace/make-build-resource resource)
    :build-fn build-game-object
    :user-data {:proto-msg proto-msg :instance-data (map :instance-data (flatten dep-build-targets))}
    :deps (flatten dep-build-targets)}])

(g/defnk produce-scene [_node-id child-scenes]
  {:node-id _node-id
   :aabb (reduce geom/aabb-union (geom/null-aabb) (filter #(not (nil? %)) (map :aabb child-scenes)))
   :children child-scenes})

(defn- attach-component [self-id comp-id]
  (let [conns [[:node-outline :child-outlines]
               [:properties :component-properties]
               [:_node-id :nodes]
               [:build-targets :dep-build-targets]
               [:ddf-message :ref-ddf]
               [:id :child-ids]
               [:scene :child-scenes]]]
    (for [[from to] conns]
      (g/connect comp-id from self-id to))))

(defn- attach-embedded-component [self-id comp-id]
  (let [conns [[:node-outline :child-outlines]
               [:ddf-message :embed-ddf]
               [:id :child-ids]
               [:scene :child-scenes]
               [:_node-id :nodes]
               [:build-targets :dep-build-targets]]]
    (for [[from to] conns]
      (g/connect comp-id from self-id to))))

(g/defnk produce-go-outline [_node-id child-outlines]
  {:node-id _node-id
   :label "Game Object"
   :icon game-object-icon
   :children child-outlines
   :child-reqs [{:node-type ComponentNode
                 :values {:embedded (comp not true?)}
                 :tx-attach-fn attach-component}
                {:node-type ComponentNode
                 :values {:embedded true?}
                 :tx-attach-fn attach-embedded-component}]})

(g/defnode GameObjectNode
  (inherits project/ResourceNode)

  (input ref-ddf g/Any :array)
  (input embed-ddf g/Any :array)
  (input child-scenes g/Any :array)
  (input child-ids g/Str :array)
  (input dep-build-targets g/Any :array)
  (input component-properties g/Any :array)

  (output node-outline outline/OutlineData :cached produce-go-outline)
  (output proto-msg g/Any :cached produce-proto-msg)
  (output save-data g/Any :cached produce-save-data)
  (output build-targets g/Any :cached produce-build-targets)
  (output scene g/Any :cached produce-scene))

(defn- gen-component-id [go-node base]
  (let [ids (g/node-value go-node :child-ids)]
    (loop [postfix 0]
      (let [id (if (= postfix 0) base (str base postfix))]
        (if (empty? (filter #(= id %) ids))
          id
          (recur (inc postfix)))))))

(defn- add-component [self project source-resource id position rotation properties]
  (let [resource-type (resource/resource-type source-resource)
        override? (contains? (:tags resource-type) :overridable-properties)
        type (if override? OverriddenComponent ReferencedComponent)
        path (if override?
               {:resource source-resource
                :overrides properties}
               source-resource)]
    (g/make-nodes (g/node-id->graph-id self)
                  [comp-node [type :id id :resource-type resource-type :position position :rotation rotation :path path]]
                  (attach-component self comp-node))))

(defn add-component-handler [self]
  (let [project (project/get-project self)
        workspace (:workspace (g/node-value self :resource))
        component-exts (map :ext (workspace/get-resource-types workspace :component))]
    (when-let [resource (first (dialogs/make-resource-dialog workspace {:ext component-exts :title "Select Component File"}))]
      (let [id (gen-component-id self (:ext (resource/resource-type resource)))
            op-seq (gensym)
            [comp-node] (g/tx-nodes-added
                          (g/transact
                            (concat
                              (g/operation-label "Add Component")
                              (g/operation-sequence op-seq)
                              (add-component self project resource id [0 0 0] [0 0 0] {}))))]
        ; Selection
        (g/transact
          (concat
            (g/operation-sequence op-seq)
            (g/operation-label "Add Component")
            (project/select project [comp-node])))))))

(handler/defhandler :add-from-file :global
  (active? [selection] (and (= 1 (count selection)) (g/node-instance? GameObjectNode (first selection))))
  (label [] "Add Component File")
  (run [selection] (add-component-handler (first selection))))

(defn- add-embedded-component [self project type data id position rotation select?]
  (let [graph (g/node-id->graph-id self)
        resource (project/make-embedded-resource project type data)]
    (g/make-nodes graph [comp-node [EmbeddedComponent :id id :position position :rotation rotation]]
      (g/connect comp-node :_node-id self :nodes)
      (if select?
        (project/select project [comp-node])
        [])
      (let [tx-data (project/make-resource-node graph project resource true {comp-node [#_[:_node-id :source-id]
                                                                                        [:_properties :source-properties]
                                                                                        [:node-outline :source-outline]
                                                                                        [:save-data :save-data]
                                                                                        [:scene :scene]
                                                                                        [:build-targets :build-targets]]
                                                                             self [[:_node-id :nodes]]})]
        (concat
          tx-data
          (if (empty? tx-data)
            []
            (attach-embedded-component self comp-node)))))))

(defn add-embedded-component-handler
  ([self]
    (let [workspace (:workspace (g/node-value self :resource))
          component-type (first (workspace/get-resource-types workspace :component))]
      (add-embedded-component-handler self component-type)))
  ([self component-type]
    (let [project (project/get-project self)
          template (workspace/template component-type)
          id (gen-component-id self (:ext component-type))]
      (g/transact
        (concat
          (g/operation-label "Add Component")
          (add-embedded-component self project (:ext component-type) template id [0 0 0] [0 0 0] true))))))
(defn add-embedded-component-handler [user-data]
  (let [self (:_node-id user-data)
        project (project/get-project self)
        component-type (:resource-type user-data)
        template (workspace/template component-type)]
    (let [id (gen-component-id self (:ext component-type))
          op-seq (gensym)
          [comp-node source-node] (g/tx-nodes-added
                                   (g/transact
                                    (concat
                                     (g/operation-sequence op-seq)
                                     (g/operation-label "Add Component")
                                     (add-embedded-component self project (:ext component-type) template id [0 0 0] [0 0 0] true))))]
      (g/transact
       (concat
        (g/operation-sequence op-seq)
        (g/operation-label "Add Component")
        ((:load-fn component-type) project source-node (io/reader (g/node-value source-node :resource)))
        (project/select project [comp-node]))))))

(defn add-embedded-component-label [user-data]
  (if-not user-data
    "Add Component"
    (let [rt (:resource-type user-data)]
      (or (:label rt) (:ext rt)))))

(defn add-embedded-component-options [self workspace user-data]
  (when (not user-data)
    (let [resource-types (workspace/get-resource-types workspace :component)]
      (mapv (fn [res-type] {:label (or (:label res-type) (:ext res-type))
                            :icon (:icon res-type)
                            :command :add
                            :user-data {:_node-id self :resource-type res-type}})
            resource-types))))

(handler/defhandler :add :global
  (label [user-data] (add-embedded-component-label user-data))
  (active? [selection] (and (= 1 (count selection)) (g/node-instance? GameObjectNode (first selection))))
  (run [user-data] (add-embedded-component-handler user-data))
  (options [selection user-data]
           (let [self (first selection)
                 workspace (:workspace (g/node-value self :resource))]
             (add-embedded-component-options self workspace user-data))))

(defn- v4->euler [v]
  (math/quat->euler (doto (Quat4d.) (math/clj->vecmath v))))

(defn load-game-object [project self resource]
  (let [project-graph (g/node-id->graph-id self)
        prototype     (protobuf/read-text GameObject$PrototypeDesc resource)]
    (concat
      (for [component (:components prototype)
            :let [source-resource (workspace/resolve-resource resource (:component component))
                  properties (into {} (map (fn [p] [(keyword (:id p)) (properties/str->go-prop (:value p) (:type p))]) (:properties component)))]]
        (add-component self project source-resource (:id component) (:position component) (v4->euler (:rotation component)) properties))
      (for [embedded (:embedded-components prototype)]
        (add-embedded-component self project (:type embedded) (:data embedded) (:id embedded) (:position embedded) (v4->euler (:rotation embedded)) false)))))

(defn register-resource-types [workspace]
  (workspace/register-resource-type workspace
                                    :ext "go"
                                    :label "Game Object"
                                    :node-type GameObjectNode
                                    :load-fn load-game-object
                                    :icon game-object-icon
                                    :view-types [:scene :text]
                                    :view-opts {:scene {:grid true}}))
